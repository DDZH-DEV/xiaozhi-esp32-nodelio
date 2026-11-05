package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"nodelio/bridge/internal/bridge"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		slog.Error("bridge stopped", "error", err)
		os.Exit(1)
	}
}
func run(args []string) error {
	command := "serve"
	openUI := len(args) == 0
	if len(args) > 0 && args[0] != "" && args[0][0] != '-' {
		command = args[0]
		args = args[1:]
	}
	if command == "version" {
		fmt.Println(bridge.Version)
		return nil
	}
	defaultDir, err := bridge.DefaultDataDir()
	if err != nil {
		return err
	}
	flags := flag.NewFlagSet("nodelio-bridge "+command, flag.ContinueOnError)
	dir := flags.String("data-dir", defaultDir, "private configuration and session directory")
	open := flags.Bool("open", openUI, "open configuration in the browser")
	legacy := flags.String("legacy-config", "", "old bridge-config.json to import explicitly")
	if err = flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() > 0 {
		return errors.New("unexpected arguments")
	}
	*dir, err = filepath.Abs(*dir)
	if err != nil {
		return err
	}
	if err = bridge.ProtectDataDir(*dir); err != nil {
		return fmt.Errorf("protect data directory: %w", err)
	}
	if command == "autostart-enable" {
		return bridge.SetStartup(*dir, true)
	}
	if command == "autostart-disable" {
		return bridge.SetStartup(*dir, false)
	}
	if command == "autostart-status" {
		fmt.Printf("%+v\n", bridge.StartupStatus(*dir))
		return nil
	}
	if command != "serve" && command != "config" && command != "import-legacy" {
		return errors.New("commands: serve, config, version, import-legacy, autostart-enable, autostart-disable, autostart-status")
	}
	if command == "config" {
		b, err := os.ReadFile(filepath.Join(*dir, "config-url.txt"))
		if err != nil {
			return errors.New("start the bridge first")
		}
		return bridge.OpenBrowser(string(b))
	}
	unlock, err := bridge.LockInstance(*dir)
	if err != nil {
		if *open {
			b, e := os.ReadFile(filepath.Join(*dir, "config-url.txt"))
			if e == nil {
				return bridge.OpenBrowser(string(b))
			}
		}
		return errors.New("bridge already running for this data directory")
	}
	defer unlock()
	logName := filepath.Join(*dir, "bridge.log")
	if st, e := os.Stat(logName); e == nil && st.Size() > 4<<20 {
		_ = os.Remove(logName + ".1")
		_ = os.Rename(logName, logName+".1")
	}
	logFile, err := os.OpenFile(logName, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer logFile.Close()
	slog.SetDefault(slog.New(slog.NewTextHandler(io.MultiWriter(os.Stderr, logFile), nil)))
	config, err := bridge.OpenConfig(*dir)
	if err != nil {
		return err
	}
	if command == "import-legacy" {
		if *legacy == "" {
			return errors.New("--legacy-config is required")
		}
		return config.ImportLegacy(*legacy)
	}
	state, err := bridge.OpenState(*dir)
	if err != nil {
		return err
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	engine := bridge.NewEngine(ctx, config, state)
	engine.Ledger, err = bridge.OpenLedger(*dir)
	if err != nil {
		return err
	}
	host := bridge.NewHost(engine)
	snapshot := config.Snapshot()
	listener, err := net.Listen("tcp", snapshot.Config.Listen)
	if err != nil {
		return fmt.Errorf("listen %s: %w", snapshot.Config.Listen, err)
	}
	app := &bridge.Server{Engine: engine, Host: host, Dir: *dir, Listen: snapshot.Config.Listen, Stop: cancel}
	server := &http.Server{Handler: app.Handler(), ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 10 * time.Second, WriteTimeout: 40 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 16 << 10}
	url := "http://" + snapshot.Config.Listen + "/#token=" + snapshot.Secrets["admin_token"]
	if err = os.WriteFile(filepath.Join(*dir, "config-url.txt"), []byte(url), 0600); err != nil {
		listener.Close()
		return err
	}
	slog.Info("NodeLio Bridge started", "version", bridge.Version, "listen", snapshot.Config.Listen, "data_dir", *dir)
	// The browser token never goes to stdout or log files. The private shortcut is local only.
	if *open {
		go func() {
			if err := bridge.OpenBrowser(url); err != nil {
				slog.Info("browser unavailable; open private config-url.txt on this machine")
			}
		}()
	}
	hostDone := make(chan struct{})
	go func() { defer close(hostDone); host.Run(ctx) }()
	serveDone := make(chan error, 1)
	go func() { serveDone <- server.Serve(listener) }()
	select {
	case err = <-serveDone:
		if errors.Is(err, http.ErrServerClosed) {
			err = nil
		}
	case <-ctx.Done():
		err = nil
	}
	cancel()
	stopCtx, stop := context.WithTimeout(context.Background(), 10*time.Second)
	defer stop()
	_ = server.Shutdown(stopCtx)
	_ = server.Close()
	<-hostDone
	engine.Wait()
	slog.Info("NodeLio Bridge stopped")
	return err
}
