//! AppContainer isolation for plugins that declare neither `net` nor `files` (Windows).
//!
//! The plugin runs in an AppContainer named after it: no network, no access to the user's files,
//! only its own folder (read and run), which is granted to the container's SID. Its stdin and
//! stdout are anonymous pipes and its stderr the log file; only those three handles are passed
//! on. It starts suspended so the runner can put it in its Job Object first.

use std::fs::File;
use std::os::windows::io::{AsRawHandle, FromRawHandle};
use std::path::Path;

use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, LocalFree, SetHandleInformation, ERROR_SUCCESS, HANDLE, HANDLE_FLAG_INHERIT,
};
use windows_sys::Win32::Security::Authorization::{
    GetNamedSecurityInfoW, SetEntriesInAclW, SetNamedSecurityInfoW, EXPLICIT_ACCESS_W, GRANT_ACCESS, NO_MULTIPLE_TRUSTEE,
    SE_FILE_OBJECT, TRUSTEE_IS_SID, TRUSTEE_IS_WELL_KNOWN_GROUP, TRUSTEE_W,
};
use windows_sys::Win32::Security::Isolation::{
    CreateAppContainerProfile, DeleteAppContainerProfile, DeriveAppContainerSidFromAppContainerName,
};
use windows_sys::Win32::Security::{
    FreeSid, ACL, DACL_SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, PSID, SECURITY_ATTRIBUTES, SECURITY_CAPABILITIES,
    SUB_CONTAINERS_AND_OBJECTS_INHERIT,
};
use windows_sys::Win32::System::Pipes::CreatePipe;
use windows_sys::Win32::System::Threading::{
    CreateProcessW, DeleteProcThreadAttributeList, InitializeProcThreadAttributeList, UpdateProcThreadAttribute,
    CREATE_NO_WINDOW, CREATE_SUSPENDED, EXTENDED_STARTUPINFO_PRESENT, LPPROC_THREAD_ATTRIBUTE_LIST, PROCESS_INFORMATION,
    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, STARTF_USESTDHANDLES, STARTUPINFOEXW,
};

const GENERIC_READ: u32 = 0x8000_0000;
const GENERIC_EXECUTE: u32 = 0x2000_0000;

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn os_err() -> std::io::Error {
    std::io::Error::last_os_error()
}

/// The container's SID, created on first use (the name is per plugin).
struct ContainerSid(PSID);

fn container_name(id: &str) -> Vec<u16> {
    let name: String = format!("foxglove.plugin.{id}").chars().filter(|c| c.is_ascii_alphanumeric() || ".-_".contains(*c)).take(64).collect();
    wide(&name)
}

/// Removes a plugin's AppContainer profile (when the plugin is uninstalled).
pub fn delete_container(id: &str) {
    let name = container_name(id);
    unsafe { DeleteAppContainerProfile(name.as_ptr()) };
}

impl ContainerSid {
    fn for_plugin(id: &str) -> std::io::Result<ContainerSid> {
        let name = container_name(id);
        let mut sid: PSID = std::ptr::null_mut();
        let display = wide("Foxglove plugin");
        let hr = unsafe { CreateAppContainerProfile(name.as_ptr(), display.as_ptr(), display.as_ptr(), std::ptr::null(), 0, &mut sid) };
        if hr < 0 {
            // Already made by an earlier run: look it up instead.
            let hr = unsafe { DeriveAppContainerSidFromAppContainerName(name.as_ptr(), &mut sid) };
            if hr < 0 {
                return Err(std::io::Error::other(format!("AppContainer profile: HRESULT {hr:#x}")));
            }
        }
        Ok(ContainerSid(sid))
    }
}

impl Drop for ContainerSid {
    fn drop(&mut self) {
        unsafe { FreeSid(self.0) };
    }
}

/// Lets the container read and run what's in the plugin's folder (inherited by everything in it).
fn grant_folder(dir: &Path, sid: &ContainerSid) -> std::io::Result<()> {
    let path = wide(&dir.to_string_lossy());
    unsafe {
        let mut old: *mut ACL = std::ptr::null_mut();
        let mut sd: PSECURITY_DESCRIPTOR = std::ptr::null_mut();
        let e = GetNamedSecurityInfoW(path.as_ptr(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, std::ptr::null_mut(), std::ptr::null_mut(), &mut old, std::ptr::null_mut(), &mut sd);
        if e != ERROR_SUCCESS {
            return Err(std::io::Error::from_raw_os_error(e as i32));
        }
        let access = EXPLICIT_ACCESS_W {
            grfAccessPermissions: GENERIC_READ | GENERIC_EXECUTE,
            grfAccessMode: GRANT_ACCESS,
            grfInheritance: SUB_CONTAINERS_AND_OBJECTS_INHERIT,
            Trustee: TRUSTEE_W {
                pMultipleTrustee: std::ptr::null_mut(),
                MultipleTrusteeOperation: NO_MULTIPLE_TRUSTEE,
                TrusteeForm: TRUSTEE_IS_SID,
                TrusteeType: TRUSTEE_IS_WELL_KNOWN_GROUP,
                ptstrName: sid.0 as *mut u16,
            },
        };
        let mut new: *mut ACL = std::ptr::null_mut();
        let e = SetEntriesInAclW(1, &access, old, &mut new);
        LocalFree(sd);
        if e != ERROR_SUCCESS {
            return Err(std::io::Error::from_raw_os_error(e as i32));
        }
        let e = SetNamedSecurityInfoW(path.as_ptr(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, std::ptr::null_mut(), std::ptr::null_mut(), new, std::ptr::null());
        LocalFree(new as _);
        if e != ERROR_SUCCESS {
            return Err(std::io::Error::from_raw_os_error(e as i32));
        }
    }
    Ok(())
}

/// A pipe, neither end inheritable yet: the child's end becomes so only for the moment the
/// process is created, and only that process gets it (the handle list).
fn pipe(child_reads: bool) -> std::io::Result<(HANDLE, File)> {
    let sa = SECURITY_ATTRIBUTES { nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32, lpSecurityDescriptor: std::ptr::null_mut(), bInheritHandle: 0 };
    let (mut read, mut write): (HANDLE, HANDLE) = (std::ptr::null_mut(), std::ptr::null_mut());
    if unsafe { CreatePipe(&mut read, &mut write, &sa, 0) } == 0 {
        return Err(os_err());
    }
    let (child, parent) = if child_reads { (read, write) } else { (write, read) };
    Ok((child, unsafe { File::from_raw_handle(parent as _) }))
}

/// A Plugin process started suspended in its AppContainer.
pub struct Contained {
    pub process: HANDLE,
    pub thread: HANDLE,
    pub stdin: File,
    pub stdout: File,
}

unsafe impl Send for Contained {}

/// Starts `program args` from `dir`, suspended: in the plugin's AppContainer when `contained`.
/// Either way the process inherits only its own pipe ends and log.
pub fn spawn(id: &str, dir: &Path, run: &[String], log: File, contained: bool) -> std::io::Result<Contained> {
    let sid = if contained {
        let sid = ContainerSid::for_plugin(id)?;
        grant_folder(dir, &sid)?;
        Some(sid)
    } else {
        None
    };
    let program = dir.join(run.first().ok_or_else(|| std::io::Error::other("the manifest's run is empty"))?);
    let mut cmdline = format!("\"{}\"", program.display());
    for a in &run[1..] {
        cmdline.push_str(&format!(" \"{}\"", a.replace('"', "\\\"")));
    }
    let mut cmdline = wide(&cmdline);
    let (child_in, stdin) = pipe(true)?;
    let (child_out, stdout) = pipe(false)?;
    let child_err = log.as_raw_handle() as HANDLE;
    let inherit = [child_in, child_out, child_err];
    for h in inherit {
        unsafe { SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) };
    }
    let attributes = if sid.is_some() { 2 } else { 1 };

    unsafe {
        let mut size = 0usize;
        InitializeProcThreadAttributeList(std::ptr::null_mut(), attributes, 0, &mut size);
        let mut buf = vec![0u8; size];
        let attrs = buf.as_mut_ptr() as LPPROC_THREAD_ATTRIBUTE_LIST;
        if InitializeProcThreadAttributeList(attrs, attributes, 0, &mut size) == 0 {
            return Err(os_err());
        }
        let caps = sid.as_ref().map(|sid| SECURITY_CAPABILITIES { AppContainerSid: sid.0, Capabilities: std::ptr::null_mut(), CapabilityCount: 0, Reserved: 0 });
        let ok = caps.as_ref().is_none_or(|caps| UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES as usize, caps as *const _ as *const _, std::mem::size_of::<SECURITY_CAPABILITIES>(), std::ptr::null_mut(), std::ptr::null())
            != 0)
            && UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST as usize, inherit.as_ptr() as *const _, std::mem::size_of_val(&inherit), std::ptr::null_mut(), std::ptr::null()) != 0;
        if !ok {
            let e = os_err();
            DeleteProcThreadAttributeList(attrs);
            return Err(e);
        }
        let mut si: STARTUPINFOEXW = std::mem::zeroed();
        si.StartupInfo.cb = std::mem::size_of::<STARTUPINFOEXW>() as u32;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = child_in;
        si.StartupInfo.hStdOutput = child_out;
        si.StartupInfo.hStdError = child_err;
        si.lpAttributeList = attrs;
        let cwd = wide(&dir.to_string_lossy());
        let mut pi: PROCESS_INFORMATION = std::mem::zeroed();
        let created = CreateProcessW(
            std::ptr::null(),
            cmdline.as_mut_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            1,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
            std::ptr::null(),
            cwd.as_ptr(),
            &si.StartupInfo,
            &mut pi,
        );
        let err = GetLastError();
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(child_in);
        CloseHandle(child_out);
        drop(log);
        if created == 0 {
            return Err(std::io::Error::from_raw_os_error(err as i32));
        }
        Ok(Contained { process: pi.hProcess, thread: pi.hThread, stdin, stdout })
    }
}
