"""Read-only Windows usage capture. No restart or configuration changes."""
import ctypes as c, ctypes.wintypes as w, time, csv, sys, datetime, os
from pathlib import Path
if len(sys.argv)<2: raise SystemExit("Usage: python capture_native.py PID [seconds]")
pid=int(sys.argv[1])
seconds=int(sys.argv[2]) if len(sys.argv)>2 else 60
k=c.WinDLL('kernel32',use_last_error=True);p=c.WinDLL('psapi',use_last_error=True)
class Perf(c.Structure):
    _fields_=[('cb',w.DWORD)]+[(s,c.c_size_t) for s in ['CommitTotal','CommitLimit','CommitPeak','PhysicalTotal','PhysicalAvailable','SystemCache','KernelTotal','KernelPaged','KernelNonpaged','PageSize']]+[(s,w.DWORD) for s in ['HandleCount','ProcessCount','ThreadCount']]
class Mem(c.Structure):
    _fields_=[('cb',w.DWORD),('PageFaultCount',w.DWORD)]+[(s,c.c_size_t) for s in ['PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage','QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage','PrivateUsage']]
class IO(c.Structure):
    _fields_=[(s,c.c_ulonglong) for s in ['ReadOperationCount','WriteOperationCount','OtherOperationCount','ReadTransferCount','WriteTransferCount','OtherTransferCount']]
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
k.GetProcessTimes.argtypes=[w.HANDLE]+[c.POINTER(c.c_ulonglong)]*4
k.GetSystemTimes.argtypes=[c.POINTER(c.c_ulonglong)]*3
k.GetProcessIoCounters.argtypes=[w.HANDLE,c.POINTER(IO)]
k.CloseHandle.argtypes=[w.HANDLE]
p.GetProcessMemoryInfo.argtypes=[w.HANDLE,c.POINTER(Mem),w.DWORD]
p.GetPerformanceInfo.argtypes=[c.POINTER(Perf),w.DWORD]
h=k.OpenProcess(0x410,False,pid)
if not h: raise c.WinError(c.get_last_error())
def snap():
    perf=Perf();perf.cb=c.sizeof(perf)
    mem=Mem();mem.cb=c.sizeof(mem);io=IO()
    idle,kernel,user=[c.c_ulonglong() for _ in range(3)]
    create,exit_,pk,pu=[c.c_ulonglong() for _ in range(4)]
    for ok in [p.GetPerformanceInfo(c.byref(perf),perf.cb),p.GetProcessMemoryInfo(h,c.byref(mem),mem.cb),k.GetProcessIoCounters(h,c.byref(io)),k.GetSystemTimes(c.byref(idle),c.byref(kernel),c.byref(user)),k.GetProcessTimes(h,c.byref(create),c.byref(exit_),c.byref(pk),c.byref(pu))]:
        if not ok: raise c.WinError(c.get_last_error())
    return time.monotonic(),perf,mem,io,idle.value,kernel.value+user.value,pk.value+pu.value
try:
    prev=snap();start=prev[0]
    with (Path(__file__).parent/'native-usage.csv').open('w',newline='') as f:
        writer=None
        while time.monotonic()-start<seconds:
            time.sleep(2);cur=snap();t,perf,mem,io,idle,total,cpu=cur;dt=t-prev[0]
            row=dict(Timestamp=datetime.datetime.now().astimezone().isoformat(),ElapsedSeconds=t-start,ProcessCPUCores=(cpu-prev[6])/1e7/dt,ProcessCPUPercent=(cpu-prev[6])/1e7/dt/(os.cpu_count() or 1)*100,SystemCPUPercent=100*(1-(idle-prev[4])/(total-prev[5])),AvailableRAMMiB=perf.PhysicalAvailable*perf.PageSize/2**20,TotalRAMMiB=perf.PhysicalTotal*perf.PageSize/2**20,CommittedMiB=perf.CommitTotal*perf.PageSize/2**20,CommitLimitMiB=perf.CommitLimit*perf.PageSize/2**20,WorkingSetMiB=mem.WorkingSetSize/2**20,PrivateBytesMiB=mem.PrivateUsage/2**20,ProcessPageFaultsPerSec=(mem.PageFaultCount-prev[2].PageFaultCount)/dt,ProcessReadMiBPerSec=(io.ReadTransferCount-prev[3].ReadTransferCount)/2**20/dt,ProcessWriteMiBPerSec=(io.WriteTransferCount-prev[3].WriteTransferCount)/2**20/dt)
            if writer is None:writer=csv.DictWriter(f,fieldnames=row);writer.writeheader()
            writer.writerow(row);f.flush();prev=cur
    print('Native CPU and memory capture complete.')
finally:k.CloseHandle(h)
