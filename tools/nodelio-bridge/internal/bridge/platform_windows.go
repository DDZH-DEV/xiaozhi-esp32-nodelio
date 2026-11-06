package bridge

import (
	"golang.org/x/sys/windows"
	"os"
	"os/exec"
	"os/user"
	"path/filepath"
	"syscall"
	"unsafe"
)

func hideCommand(c *exec.Cmd) { c.SysProcAttr = &syscall.SysProcAttr{HideWindow: true} }
func ProtectDataDir(dir string) error {
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	u, err := user.Current()
	if err != nil {
		return err
	}
	// An explicit per-user ACL is required: os.FileMode does not restrict Windows access.
	// The directory is owned by Administrators. A non-elevated air restart cannot run
	// icacls /inheritance:r again (exit 5) once the folder is already private.
	if ok, checkErr := dirAllowsOnly(dir, u.Uid); checkErr == nil && ok {
		return nil
	}
	return runCommand("icacls.exe", dir, "/inheritance:r", "/grant:r", "*"+u.Uid+":(OI)(CI)F")
}
func dirAllowsOnly(dir, sid string) (bool, error) {
	sd, err := windows.GetNamedSecurityInfo(dir, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return false, err
	}
	dacl, _, err := sd.DACL()
	if err != nil || dacl == nil || dacl.AceCount == 0 {
		return false, err
	}
	want, err := windows.StringToSid(sid)
	if err != nil {
		return false, err
	}
	for i := uint16(0); i < dacl.AceCount; i++ {
		var ace *windows.ACCESS_ALLOWED_ACE
		if err = windows.GetAce(dacl, uint32(i), &ace); err != nil {
			return false, err
		}
		if ace.Header.AceType != windows.ACCESS_ALLOWED_ACE_TYPE {
			return false, nil
		}
		if !(*windows.SID)(unsafe.Pointer(&ace.SidStart)).Equals(want) {
			return false, nil
		}
	}
	return true, nil
}
func LockInstance(dir string) (func(), error) {
	f, err := os.OpenFile(filepath.Join(dir, "instance.lock"), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	o := &windows.Overlapped{}
	if err = windows.LockFileEx(windows.Handle(f.Fd()), windows.LOCKFILE_EXCLUSIVE_LOCK|windows.LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, o); err != nil {
		f.Close()
		return nil, err
	}
	return func() { windows.UnlockFileEx(windows.Handle(f.Fd()), 0, 1, 0, o); f.Close() }, nil
}
