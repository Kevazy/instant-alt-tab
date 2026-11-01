"""Finds the Windows internals that Instant Alt+Tab hooks and prints them in the
layout of src/hook/pins.h, for the DLLs of this machine or given copies.

twinui.pcshell.dll has public symbols, so its functions are found by name (the
PDB is downloaded from the Microsoft symbol server into --symbols). The switcher
DLL has none: its functions and offsets are found by code structure. Anything
that can't be derived is reported for a manual check (see docs/internals.md).

Requires Python 3.10+ and the Debugging Tools for Windows (dbghelp.dll and
symsrv.dll, installed with the Windows SDK).

usage: python tools/find_offsets.py [--twinui PATH] [--switcher PATH] [--symbols DIR]
"""

import argparse
import ctypes as c
import ctypes.wintypes as w
import os
import re
import struct
import sys

SYSTEM = os.environ.get('SystemRoot', r'C:\Windows')
DEFAULT_TWINUI = os.path.join(SYSTEM, r'System32\twinui.pcshell.dll')
DEFAULT_SWITCHER = os.path.join(SYSTEM, r'ShellComponents\WindowsInternal.ComposableShell.Experiences.Switcher.dll')
DEBUGGERS = r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64'
SEARCHED_TYPE = bytes.fromhex('d86d50ae5cf7ce3cbe60d9827ab2444e')  # {ae506dd8-f75c-3cce-be60-d9827ab2444e}


class Image:
    """A PE file on disk, addressed by RVA."""

    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        pe = struct.unpack_from('<I', self.data, 0x3c)[0]
        optional = pe + 24
        self.timestamp = struct.unpack_from('<I', self.data, pe + 8)[0]
        self.size = struct.unpack_from('<I', self.data, optional + 56)[0]
        count = struct.unpack_from('<H', self.data, pe + 6)[0]
        optional_size = struct.unpack_from('<H', self.data, pe + 20)[0]
        self.sections = []
        for i in range(count):
            name, vsize, va, rsize, raw = struct.unpack_from('<8sIIII', self.data, optional + optional_size + i * 40)
            self.sections.append((name.rstrip(b'\0').decode(), va, max(vsize, rsize), raw))
        _, self.text_rva, self.text_size, text_raw = next(s for s in self.sections if s[0] == '.text')
        self.code = self.data[text_raw:text_raw + self.text_size]
        exception_rva, exception_size = struct.unpack_from('<II', self.data, optional + 112 + 3 * 8)
        self.functions = [struct.unpack_from('<III', self.data, self.offset(exception_rva) + i * 12)
                          for i in range(exception_size // 12)]

    def offset(self, rva):
        for _, va, size, raw in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f'RVA 0x{rva:x} is outside every section')

    def rva(self, offset):
        for _, va, size, raw in self.sections:
            if raw <= offset < raw + size:
                return offset - raw + va
        raise ValueError(f'file offset 0x{offset:x} is outside every section')

    def read(self, rva, count):
        start = self.offset(rva)
        return self.data[start:start + count]

    def function_of(self, rva):
        """Start of the function containing rva, following chained unwind info."""
        for begin, end, unwind in self.functions:
            if begin <= rva < end:
                while True:
                    info = self.offset(unwind & ~1)
                    if not (self.data[info] >> 3) & 4:  # UNW_FLAG_CHAININFO
                        return begin
                    count = self.data[info + 2]
                    begin, end, unwind = struct.unpack_from('<III', self.data, info + 4 + 2 * ((count + 1) & ~1))
        return None

    def function_end(self, start):
        return next(end for begin, end, _ in self.functions if begin == start)

    def call_target(self, call_rva):
        return call_rva + 5 + struct.unpack_from('<i', self.read(call_rva + 1, 4))[0]

    def calls(self):
        """Every direct call in .text, by target."""
        targets = {}
        for match in re.finditer(b'\xe8', self.code):
            i = match.start()
            if i + 5 <= len(self.code):
                call_rva = self.text_rva + i
                targets.setdefault(self.call_target(call_rva), []).append(call_rva)
        return targets


def unique(what, values):
    values = sorted(set(values))
    if len(values) != 1:
        sys.exit(f'{what}: expected one match, found {[hex(v) for v in values]}')
    return values[0]


def disp32(data, at):
    return struct.unpack_from('<i', data, at)[0]


# --- twinui.pcshell.dll: by symbol name -----------------------------------------------------------

class Symbols:
    class Info(c.Structure):
        _fields_ = [('SizeOfStruct', w.ULONG), ('TypeIndex', w.ULONG), ('Reserved', c.c_ulonglong * 2),
                    ('Index', w.ULONG), ('Size', w.ULONG), ('ModBase', c.c_ulonglong), ('Flags', w.ULONG),
                    ('Value', c.c_ulonglong), ('Address', c.c_ulonglong), ('Register', w.ULONG),
                    ('Scope', w.ULONG), ('Tag', w.ULONG), ('NameLen', w.ULONG), ('MaxNameLen', w.ULONG),
                    ('Name', c.c_wchar * 1)]

    Callback = c.WINFUNCTYPE(w.BOOL, c.POINTER(Info), w.ULONG, c.c_void_p)

    def __init__(self, image, symbol_dir):
        os.add_dll_directory(DEBUGGERS)
        self.dbghelp = c.WinDLL(os.path.join(DEBUGGERS, 'dbghelp.dll'), use_last_error=True)
        self.dbghelp.SymInitializeW.argtypes = [w.HANDLE, w.LPCWSTR, w.BOOL]
        self.dbghelp.SymLoadModuleExW.argtypes = [w.HANDLE, w.HANDLE, w.LPCWSTR, w.LPCWSTR, c.c_ulonglong, w.DWORD,
                                                  c.c_void_p, w.DWORD]
        self.dbghelp.SymLoadModuleExW.restype = c.c_ulonglong
        self.dbghelp.SymEnumSymbolsW.argtypes = [w.HANDLE, c.c_ulonglong, w.LPCWSTR, Symbols.Callback, c.c_void_p]
        self.process = w.HANDLE(0x1A17AB)  # any unique value identifies the dbghelp session
        self.dbghelp.SymSetOptions(0x2 | 0x200 | 0x80000)  # undecorated names, deferred loads, no prompts
        path = f'{symbol_dir};srv*{symbol_dir}*https://msdl.microsoft.com/download/symbols'
        self.dbghelp.SymInitializeW(self.process, path, False)
        self.base = self.dbghelp.SymLoadModuleExW(self.process, None, image.path, None, 0, 0, None, 0)
        if not self.base:
            sys.exit(f'cannot load symbols for {image.path} (error {c.get_last_error()})')

    def find(self, mask):
        found = []

        @Symbols.Callback
        def visit(info, size, context):
            name = c.wstring_at(c.addressof(info.contents) + Symbols.Info.Name.offset, info.contents.NameLen)
            name = name.rstrip(chr(0))  # NameLen can include the terminator
            found.append((info.contents.Address - self.base, name))
            return True

        self.dbghelp.SymEnumSymbolsW(self.process, self.base, mask, visit, None)
        return found

    def one(self, mask):
        return unique(mask, [rva for rva, _ in self.find(mask)])


def find_twinui(image, symbol_dir):
    symbols = Symbols(image, symbol_dir)
    found = {
        'kCreateControl': symbols.one('XamlAltTabViewHost::CreateAltTabControl'),
        'kShow': symbols.one('XamlAltTabViewHost::Show'),
        'kDismiss': symbols.one('XamlAltTabViewHost::v_DismissView'),
        'kDisplay': symbols.one('XamlAltTabViewHost::DisplayAltTab'),
        'kGetItemForView': symbols.one('CachedDataSource::GetItemForView'),
        'kRegisterRoot': symbols.one('*IHostInformationRegistrarPublicNonVirtuals::RegisterXamlRoot'),
    }
    # The content setter: the folded IXamlExplorerHostWindow XamlContent consumer
    # that calls vtable slot 9 (mov rax,[rax+48h]).
    found['kSetHostContent'] = unique('content setter', [
        rva for rva, _ in symbols.find('*IXamlExplorerHostWindow>::XamlContent')
        if bytes.fromhex('488b4048') in image.read(rva, 0x30)])

    show = found['kShow']
    show_code = image.read(show, image.function_end(show) - show)
    calls_in_show = [(show + i, image.call_target(show + i)) for i in range(len(show_code) - 5) if show_code[i] == 0xE8]
    root_return = unique('RegisterXamlRoot call in Show',
                         [at + 5 for at, target in calls_in_show if target == found['kRegisterRoot']])
    # The control member: "lea rsi,[rdi+X]" right after the factory call.
    factory_call = unique('factory call in Show', [at for at, target in calls_in_show if target == found['kCreateControl']])
    after = image.read(factory_call + 5, 0x20)
    control = disp32(after, after.index(b'\x48\x8d\xb7') + 3)
    # The host window member: "lea r12,[rdi+X]", the register passed to the content setter.
    window = disp32(show_code, show_code.index(b'\x4c\x8d\xa7') + 3)
    # The data source member: the factory's first "lea rcx,[rdi+X]".
    factory_code = image.read(found['kCreateControl'], 0x200)
    data_source = disp32(factory_code, factory_code.index(b'\x48\x8d\x8f') + 3)
    # Show's backward flag ("test r15b,X"), then the wrappers that call the control's
    # NavigateToItem and Navigate, in that order ("mov rax,[rax+slot*8]").
    flag_at = show_code.index(b'\x41\xf6\xc7')
    backward = show_code[flag_at + 3]
    slots = []
    for at, target in calls_in_show:
        if at < show + flag_at or len(slots) == 2:
            continue
        try:
            wrapper_code = image.read(target, 0x20)
        except ValueError:  # an E8 byte that isn't a call
            continue
        if b'\x48\x8b\x80' in wrapper_code:
            slots.append(disp32(wrapper_code, wrapper_code.index(b'\x48\x8b\x80') + 3) // 8)

    # The window's own get/put_XamlContent (not the WASDK variant, not exception funclets).
    content = [(rva, name) for rva, name in symbols.find('*XamlExplorerHostWindow*XamlContent*')
               if 'produce' in name and '_WASDK' not in name]
    getter = unique('content getter', [rva for rva, name in content if name.endswith('::get_XamlContent')])
    setter = unique('content setter implementation', [rva for rva, name in content if name.endswith('::put_XamlContent')])
    return found, dict(root_return=root_return, root_call=image.read(root_return - 5, 5), control=control,
                       window=window, data_source=data_source, backward=backward, slots=slots, getter=getter,
                       setter=setter)


# --- Switcher: by code structure ------------------------------------------------------------------

def find_switcher(image):
    code, base = image.code, image.text_rva
    calls = image.calls()

    def before(rva, count):
        return image.read(rva - count, count)

    def mov_edx(value):
        return b'\xba' + struct.pack('<I', value)

    # The vector change notifier: "mov ebx,r8d; mov edi,edx; mov rsi,rcx; cmp byte [rcx+X],0; je" near its
    # start (X = the observed flag). RemoveAt calls it with change 2, InsertAt with change 1, and the list
    # re-sync is the one function calling both on [rsi+member] and the sort ("mov rcx,rsi; call sort").
    shape = re.compile(re.escape(bytes.fromhex('418bd88bfa488bf18079')) + b'(.)' + re.escape(b'\x00\x0f\x84'), re.S)
    results = []
    for match in shape.finditer(code):
        notifier = image.function_of(base + match.start())
        if notifier is None or base + match.start() - notifier >= 0x20:
            continue
        removers = {image.function_of(at) for at in calls.get(notifier, []) if mov_edx(2) in before(at, 12)}
        inserters = {image.function_of(at) for at in calls.get(notifier, []) if mov_edx(1) in before(at, 12)}
        users = {}
        for kind, functions in (('remove', removers), ('insert', inserters)):
            for function in functions:
                for at in calls.get(function, []):
                    load = before(at, 7)
                    if load[:3] == b'\x48\x8b\x8e':  # mov rcx,[rsi+member]
                        users.setdefault(image.function_of(at), {}).setdefault(kind, set()).add(
                            (function, struct.unpack_from('<I', load, 3)[0]))
        for sync, kinds in users.items():
            members = {member for pairs in kinds.values() for _, member in pairs}
            if set(kinds) != {'remove', 'insert'} or len(members) != 1:
                continue
            body = image.read(sync, image.function_end(sync) - sync)
            sorts = {image.call_target(sync + m.start() + 3) for m in re.finditer(re.escape(b'\x48\x8b\xce\xe8'), body)}
            sorts = {s for s in sorts if image.read(s, 5) == bytes.fromhex('48895c2410')}
            if len(sorts) == 1:
                results.append(dict(kVectorNotify=notifier, observed=match.group(1)[0], kSyncList=sync,
                                    cards=members.pop(), kSortList=sorts.pop(),
                                    kVectorRemoveAt=unique('RemoveAt', [f for f, _ in kinds['remove']]),
                                    kVectorInsertAt=unique('InsertAt', [f for f, _ in kinds['insert']])))
    if len(results) != 1:
        sys.exit(f'card vector: expected one match, found {len(results)}')
    found = results[0]
    remove_at = image.read(found['kVectorRemoveAt'], 0x40)
    found['storage'] = remove_at[re.search(re.escape(b'\x48\x8b\x47'), remove_at).end()]  # mov rax,[rdi+storage]

    # "Card chosen": "mov byte [rcx+closed],1; jmp" into a function that unregisters the list
    # ("sub rsp,28h; cmp qword [rcx+token],0; ...; mov rcx,[rcx+source]").
    close = []
    for match in re.finditer(re.escape(b'\xc6\x81') + b'(..)' + re.escape(b'\x00\x00\x01\xe9'), code, re.S):
        jump = base + match.end() - 1
        target = image.call_target(jump)
        target_code = image.read(target, 0x30)
        if target_code.startswith(bytes.fromhex('4883ec284883b9')) and b'\x48\x8b\x89' in target_code:
            close.append((base + match.start(), struct.unpack_from('<H', match.group(1))[0]))
    if len(close) != 1:
        sys.exit(f'close list: expected one match, found {len(close)}')
    found['kCloseList'], found['closed'] = close[0]

    # The descendant search: the recursive function that references the searched type's IID.
    guid_rva = image.rva(image.data.index(SEARCHED_TYPE))
    referencing = set()
    for i in range(1, len(code) - 4):
        if code[i - 1] & 0xC7 == 0x05 and base + i + 4 + disp32(code, i) == guid_rva:  # [rip+disp32]
            if (function := image.function_of(base + i)) is not None:
                referencing.add(function)
    recursive = [f for f in referencing if any(image.function_of(at) == f for at in calls.get(f, []))]
    found['kFindDescendant'] = unique('descendant search', recursive)
    return found


# --- Output ---------------------------------------------------------------------------------------

def entry(image, rva):
    return ', '.join(f'0x{b:02X}' for b in image.read(rva, 16))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--twinui', default=DEFAULT_TWINUI)
    parser.add_argument('--switcher', default=DEFAULT_SWITCHER)
    parser.add_argument('--symbols', default=os.path.join(os.environ.get('TEMP', '.'), 'symbols'))
    args = parser.parse_args()

    twinui, switcher = Image(args.twinui), Image(args.switcher)
    functions, extra = find_twinui(twinui, args.symbols)
    found = find_switcher(switcher)

    print(f'// {args.twinui}')
    print(f'inline constexpr Module kTwinui{{L"twinui.pcshell.dll", 0x{twinui.timestamp:x}, 0x{twinui.size:x}}};')
    for name, rva in functions.items():
        print(f'inline constexpr Function {name}{{0x{rva:x}, {{{entry(twinui, rva)}}}}};')
    print(f'inline constexpr DWORD kRegisterRootReturnInShow = 0x{extra["root_return"]:x};')
    print(f'inline constexpr std::array<BYTE, 5> kRegisterRootCallInShow{{'
          f'{", ".join(f"0x{b:02X}" for b in extra["root_call"])}}};')
    print(f'inline constexpr DWORD kHostContentGetter = 0x{extra["getter"]:x}, kHostContentSetter = 0x{extra["setter"]:x};')
    print(f'inline constexpr size_t kHostDataSource = 0x{extra["data_source"]:x}, kHostControl = 0x{extra["control"]:x}, '
          f'kHostWindow = 0x{extra["window"]:x};')
    print(f'inline constexpr int kShowBackward = 0x{extra["backward"]:x};')
    print(f'inline constexpr size_t kNavigateToItemSlot = {extra["slots"][0]}, kNavigateSlot = {extra["slots"][1]};')
    print()
    print(f'// {args.switcher}')
    print(f'inline constexpr Module kSwitcher{{L"WindowsInternal.ComposableShell.Experiences.Switcher.dll", '
          f'0x{switcher.timestamp:x}, 0x{switcher.size:x}}};')
    for name in ['kSyncList', 'kSortList', 'kCloseList', 'kFindDescendant', 'kVectorRemoveAt', 'kVectorInsertAt',
                 'kVectorNotify']:
        print(f'inline constexpr Function {name}{{0x{found[name]:x}, {{{entry(switcher, found[name])}}}}};')
    print(f'inline constexpr size_t kListCards = 0x{found["cards"]:x}, kListClosed = 0x{found["closed"]:x};')
    print(f'inline constexpr size_t kVectorStorage = 0x{found["storage"]:x}, kVectorObserved = 0x{found["observed"]:x};')
    print()
    print('// Not derived, check by hand: kHostContentGetterSlot/kHostContentSetterSlot, kCardSwitchItem, and')
    print('// kNavigateToItem/kNavigate (the AltTab control vtable entries at kNavigateToItemSlot/kNavigateSlot).')
    print('// kISwitchItem and kSearchedType are interface IDs; they change only if the interfaces do.')


if __name__ == '__main__':
    main()
