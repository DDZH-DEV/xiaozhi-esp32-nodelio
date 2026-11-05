//go:build linux || darwin

package bridge

import (
	"os"
	"os/exec"
	"syscall"
)

func hideCommand(*exec.Cmd) {}
func ProtectDataDir(dir string) error {
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	return os.Chmod(dir, 0700)
}
func LockInstance(dir string) (func(), error) {
	f, err := os.OpenFile(dir+"/instance.lock", os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err = syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		f.Close()
		return nil, err
	}
	return func() { syscall.Flock(int(f.Fd()), syscall.LOCK_UN); f.Close() }, nil
}
