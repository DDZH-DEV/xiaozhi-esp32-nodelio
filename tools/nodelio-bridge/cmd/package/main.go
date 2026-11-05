// package builds CGO-free binaries and portable archives. It never installs or publishes them.
package main

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"nodelio/bridge/internal/bridge"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
func run() error {
	out := flag.String("out", "dist", "archive directory")
	targets := flag.String("targets", "windows/amd64,windows/arm64,darwin/amd64,darwin/arm64,linux/amd64,linux/arm64", "comma-separated OS/architecture pairs")
	flag.Parse()
	if err := os.MkdirAll(*out, 0755); err != nil {
		return err
	}
	var checksums []string
	for _, target := range strings.Split(*targets, ",") {
		pair := strings.Split(target, "/")
		if len(pair) != 2 {
			return fmt.Errorf("invalid target: %s", target)
		}
		goos, arch := pair[0], pair[1]
		if (goos != "windows" && goos != "darwin" && goos != "linux") || (arch != "amd64" && arch != "arm64") {
			return fmt.Errorf("unsupported target: %s", target)
		}
		stage, err := os.MkdirTemp("", "nodelio-package-")
		if err != nil {
			return err
		}
		defer os.RemoveAll(stage)
		binary := "nodelio-bridge"
		if goos == "windows" {
			binary += ".exe"
		}
		dest := filepath.Join(stage, binary)
		if goos == "darwin" {
			dest = filepath.Join(stage, "NodeLio Bridge.app", "Contents", "MacOS", "nodelio-bridge")
			if err = os.MkdirAll(filepath.Dir(dest), 0755); err != nil {
				return err
			}
		}
		ldflags := "-s -w"
		if goos == "windows" {
			ldflags += " -H=windowsgui"
		}
		goExe := filepath.Join(runtime.GOROOT(), "bin", "go")
		if runtime.GOOS == "windows" {
			goExe += ".exe"
		}
		cmd := exec.Command(goExe, "build", "-trimpath", "-ldflags", ldflags, "-o", dest, "./cmd/nodelio-bridge")
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		for _, env := range os.Environ() {
			key := strings.SplitN(env, "=", 2)[0]
			if key != "GOOS" && key != "GOARCH" && key != "CGO_ENABLED" {
				cmd.Env = append(cmd.Env, env)
			}
		}
		cmd.Env = append(cmd.Env, "GOOS="+goos, "GOARCH="+arch, "CGO_ENABLED=0")
		fmt.Println("Building", target)
		if err = cmd.Run(); err != nil {
			return err
		}
		if goos == "darwin" {
			plist := `<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>CFBundleIdentifier</key><string>com.nodelio.bridge</string><key>CFBundleName</key><string>NodeLio Bridge</string><key>CFBundleExecutable</key><string>nodelio-bridge</string><key>CFBundlePackageType</key><string>APPL</string><key>CFBundleShortVersionString</key><string>` + bridge.Version + `</string><key>LSUIElement</key><true/></dict></plist>`
			if err = os.WriteFile(filepath.Join(stage, "NodeLio Bridge.app", "Contents", "Info.plist"), []byte(plist), 0644); err != nil {
				return err
			}
		}
		for _, name := range []string{"README.md"} {
			b, err := os.ReadFile(name)
			if err != nil {
				return err
			}
			if err = os.WriteFile(filepath.Join(stage, name), b, 0644); err != nil {
				return err
			}
		}
		installer := "install.sh"
		if goos == "windows" {
			installer = "install.ps1"
		}
		b, err := os.ReadFile(filepath.Join("packaging", goos, installer))
		if err != nil {
			return err
		}
		if err = os.WriteFile(filepath.Join(stage, installer), b, 0755); err != nil {
			return err
		}
		name := fmt.Sprintf("nodelio-bridge-%s-%s-%s", bridge.Version, goos, arch)
		extension := ".tar.gz"
		if goos == "windows" || goos == "darwin" {
			extension = ".zip"
		}
		archive := filepath.Join(*out, name+extension)
		if err = pack(stage, archive, name, extension == ".zip"); err != nil {
			return err
		}
		f, err := os.Open(archive)
		if err != nil {
			return err
		}
		hash := sha256.New()
		_, err = io.Copy(hash, f)
		f.Close()
		if err != nil {
			return err
		}
		checksums = append(checksums, hex.EncodeToString(hash.Sum(nil))+"  "+filepath.Base(archive))
	}
	sort.Strings(checksums)
	return os.WriteFile(filepath.Join(*out, "SHA256SUMS"), []byte(strings.Join(checksums, "\n")+"\n"), 0644)
}
func pack(root, dest, prefix string, isZip bool) error {
	f, err := os.Create(dest)
	if err != nil {
		return err
	}
	defer f.Close()
	var z *zip.Writer
	var g *gzip.Writer
	var tw *tar.Writer
	if isZip {
		z = zip.NewWriter(f)
	} else {
		g = gzip.NewWriter(f)
		tw = tar.NewWriter(g)
	}
	err = filepath.WalkDir(root, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			return nil
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		name := prefix + "/" + filepath.ToSlash(rel)
		mode := os.FileMode(0644)
		if entry.Name() == "nodelio-bridge" || entry.Name() == "install.sh" {
			mode = 0755
		}
		var w io.Writer
		if isZip {
			h := &zip.FileHeader{Name: name, Method: zip.Deflate}
			h.SetMode(mode)
			w, err = z.CreateHeader(h)
		} else {
			err = tw.WriteHeader(&tar.Header{Name: name, Mode: int64(mode), Size: info.Size()})
			w = tw
		}
		if err != nil {
			return err
		}
		in, err := os.Open(path)
		if err != nil {
			return err
		}
		defer in.Close()
		_, err = io.Copy(w, in)
		return err
	})
	if err != nil {
		return err
	}
	if isZip {
		return z.Close()
	}
	if err = tw.Close(); err != nil {
		return err
	}
	return g.Close()
}
