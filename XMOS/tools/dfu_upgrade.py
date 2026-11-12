"""Windows WinUSB uploader for the lib_xua runtime DFU interface (64-byte blocks)."""
import argparse
import ctypes as C
from ctypes import wintypes as W
from pathlib import Path
import time
import uuid

cfg = C.WinDLL('cfgmgr32', use_last_error=True)
kernel = C.WinDLL('kernel32', use_last_error=True)
usb = C.WinDLL('winusb', use_last_error=True)
guid = (C.c_ubyte * 16).from_buffer_copy(uuid.UUID('89C14132-D389-4FF7-944E-2E33379BB59D').bytes_le)
cfg.CM_Get_Device_Interface_List_SizeW.argtypes = [C.POINTER(W.ULONG), C.c_void_p, W.LPCWSTR, W.ULONG]
cfg.CM_Get_Device_Interface_ListW.argtypes = [C.c_void_p, W.LPCWSTR, W.LPWSTR, W.ULONG, W.ULONG]
kernel.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
kernel.CreateFileW.restype = W.HANDLE
kernel.CloseHandle.argtypes = [W.HANDLE]
usb.WinUsb_Initialize.argtypes = [W.HANDLE, C.POINTER(W.HANDLE)]
usb.WinUsb_Free.argtypes = [W.HANDLE]

class Setup(C.Structure):
    _pack_ = 1
    _fields_ = [('kind', C.c_ubyte), ('request', C.c_ubyte), ('value', W.USHORT), ('index', W.USHORT), ('length', W.USHORT)]

class Interface(C.Structure):
    _pack_ = 1
    _fields_ = [(name, C.c_ubyte) for name in ('length', 'type', 'number', 'alt', 'endpoints', 'cls', 'subclass', 'protocol', 'string')]

usb.WinUsb_QueryInterfaceSettings.argtypes = [W.HANDLE, C.c_ubyte, C.POINTER(Interface)]
usb.WinUsb_ControlTransfer.argtypes = [W.HANDLE, Setup, C.c_void_p, W.ULONG, C.POINTER(W.ULONG), C.c_void_p]

def paths():
    size = W.ULONG()
    if cfg.CM_Get_Device_Interface_List_SizeW(C.byref(size), guid, None, 0) != 0:
        return []
    data = C.create_unicode_buffer(max(1, size.value))
    if cfg.CM_Get_Device_Interface_ListW(guid, None, data, max(1, size.value), 0) != 0:
        return []
    return [p for p in data[:].split(chr(0)) if 'vid_20b1' in p.lower()]

class Device:
    def __init__(self, path):
        self.handle = kernel.CreateFileW(path, 0xC0000000, 3, None, 3, 0x40000000, None)
        if self.handle == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        self.itf = W.HANDLE()
        if not usb.WinUsb_Initialize(self.handle, C.byref(self.itf)):
            kernel.CloseHandle(self.handle)
            raise C.WinError(C.get_last_error())
        self.desc = Interface()
        if not usb.WinUsb_QueryInterfaceSettings(self.itf, 0, C.byref(self.desc)):
            self.close()
            raise C.WinError(C.get_last_error())

    def transfer(self, kind, request, value=0, data=b'', length=0):
        size = length if kind & 0x80 else len(data)
        buffer = (C.c_ubyte * max(size, 1))()
        if data:
            C.memmove(buffer, data, len(data))
        used = W.ULONG()
        setup = Setup(kind, request, value, self.desc.number, size)
        if not usb.WinUsb_ControlTransfer(self.itf, setup, buffer, size, C.byref(used), None):
            raise C.WinError(C.get_last_error())
        if used.value != size:
            raise RuntimeError(f'Short control transfer: {used.value}/{size}')
        return bytes(buffer[:used.value])

    def close(self):
        if self.itf:
            usb.WinUsb_Free(self.itf)
            self.itf = W.HANDLE()
        if self.handle:
            kernel.CloseHandle(self.handle)
            self.handle = None

def wait_device(pid, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for path in paths():
            if f'pid_{pid:04x}' in path.lower():
                return Device(path)
        time.sleep(.25)
    raise RuntimeError(f'USB {pid:04x} did not enumerate within {seconds}s')

def download_idle(dev):
    for _ in range(100):
        data = dev.transfer(0xA1, 3, length=6)
        status, state = data[0], data[4]
        if status or state == 10:
            raise RuntimeError(f'DFU error status={status}, state={state}')
        if state == 5:
            return
        time.sleep(max(.001, int.from_bytes(data[1:4], 'little') / 1000))
    raise RuntimeError('Timed out waiting for flash write')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    if not args.image.name.endswith('_dfu.bin'):
        raise SystemExit('Use the xflash --upgrade image ending in _dfu.bin, not the factory image')
    image = args.image.read_bytes()
    if not image or len(image) > 0x100000:
        raise SystemExit('Invalid upgrade image size')
    dev = None
    try:
        existing = [p for p in paths() if 'pid_d016' in p.lower()]
        if existing:
            dev = Device(existing[0])
        else:
            dev = wait_device(0x0016)
            if dev.desc.protocol != 1:
                raise RuntimeError('Not a runtime DFU interface')
            print('Entering DFU mode', flush=True)
            dev.transfer(0x21, 0, 1000)
            dev.close()
            dev = wait_device(0xD016)
        if dev.desc.protocol != 2:
            raise RuntimeError('Not a DFU download interface')
        print(f'Downloading {len(image)} bytes', flush=True)
        for offset in range(0, len(image), 64):
            dev.transfer(0x21, 1, offset // 64, image[offset:offset + 64])
            download_idle(dev)
            if offset % 8192 == 0:
                print(f'{offset}/{len(image)}', flush=True)
        dev.transfer(0x21, 1)
        result = dev.transfer(0xA1, 3, length=6)
        if result[0] or result[4] not in (2, 6, 7, 8):
            raise RuntimeError(f'Failed DFU manifestation: {result.hex()}')
        print('Download complete; returning to runtime', flush=True)
        dev.transfer(0x21, 0, 1000)
        dev.close()
        dev = wait_device(0x0016)
        print('Runtime USB enumerated successfully', flush=True)
    finally:
        if dev:
            dev.close()

if __name__ == '__main__':
    main()
