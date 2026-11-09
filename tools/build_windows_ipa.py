#!/usr/bin/env python3
"""
build_windows_ipa.py
Cross-compiles libSappleMultilang.dylib (ARM64 iOS) on Windows using Zig CC
(including OneCore, SAPI 5 Custom Voices, Natural Voices, and IBM ProTALKER 97
読み秀君 / 読み子ちゃん via SAPI4/emu), patches the Sapple and ClassicVoicesExtension
Mach-O binaries, and packages modular Language Pack IPAs:
1. Sapple-LangPack1-JA-EN.ipa:
   Japanese (ProTalker 読み秀君 & 読み子ちゃん, OneCore Ayumi/Haruka/Ichiro/Sayaka,
   Natural Nanami/Keita) + English (OneCore + Cortana + Natural) + Classic voices
   (SAPI 5 + Custom 6 voices + SAPI 4 + TruVoice + Anna).
2. Sapple-LangPack2-EastAsia.ipa:
   East / South / Southeast Asian languages (zh-CN, zh-TW, zh-HK, ko-KR, hi-IN,
   ta-IN, th-TH, vi-VN, id-ID, ms-MY).
3. Sapple-LangPack3-WestEurope.ipa:
   Western / Southern European & Latin American languages (de, fr, es, it, pt, nl, ca).
4. Sapple-LangPack4-EastEurope-ME.ipa:
   Northern / Eastern European & Middle Eastern languages (ru, pl, cs, sk, hu, ro, bg,
   hr, sl, el, tr, da, nb, sv, fi, ar, he).
Opening Sapple once after installing any Language Pack IPA automatically copies that
pack's voice files into the shared App Group container so all installed packs work
together in both Sapple and iOS VoiceOver.
"""
import glob
import os
import re
import shutil
import struct
import subprocess
import zipfile

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRATCH_ROOT = os.path.dirname(REPO_DIR)
ZIG_EXE = os.path.join(SCRATCH_ROOT, "zig", "zig-windows-x86_64-0.13.0", "zig.exe")
BASE_IPA = os.path.join(SCRATCH_ROOT, "Sapple-1.1.1.ipa")

OUT_PACK1_IPA = os.path.join(SCRATCH_ROOT, "Sapple-LangPack1-JA-EN.ipa")
OUT_PACK2_IPA = os.path.join(SCRATCH_ROOT, "Sapple-LangPack2-EastAsia.ipa")
OUT_PACK3_IPA = os.path.join(SCRATCH_ROOT, "Sapple-LangPack3-WestEurope.ipa")
OUT_PACK4_IPA = os.path.join(SCRATCH_ROOT, "Sapple-LangPack4-EastEurope-ME.ipa")
OUT_IPA = os.path.join(SCRATCH_ROOT, "Sapple-AllVoices.ipa")
OUT_FULL_IPA = os.path.join(SCRATCH_ROOT, "Sapple-AllVoices-Full.ipa")

ICLOUD_DIR = r"C:\Users\nuigu\iCloudDrive"
ICLOUD_DL_DIR = r"C:\Users\nuigu\iCloudDrive\Downloads"

def encode_small_str_bytes(s: str) -> bytes:
    b = s.encode("utf-8")
    assert len(b) <= 15, f"String too long for SmallString: {s!r}"
    is_ascii = all(c < 0x80 for c in b)
    buf = bytearray(16)
    buf[:len(b)] = b
    buf[15] = (0xE0 if is_ascii else 0xA0) | len(b)
    return bytes(buf)

def encode_tuple(name: str, female: bool, lang: str) -> bytes:
    return (
        encode_small_str_bytes(name)
        + struct.pack("<Q", 1 if female else 0)
        + encode_small_str_bytes(lang)
    )

def arm64_adrp(rd: int, pc: int, target_page: int) -> int:
    imm = (target_page >> 12) - (pc >> 12)
    immlo = imm & 0x3
    immhi = (imm >> 2) & 0x7FFFF
    return (1 << 31) | (immlo << 29) | (0b10000 << 24) | (immhi << 5) | (rd & 0x1F)

def arm64_add_imm(rd: int, rn: int, imm12: int) -> int:
    return (1 << 31) | (0b001000100 << 22) | (imm12 << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F)

def arm64_movz_w(rd: int, imm16: int) -> int:
    return (0b01010010100 << 21) | (imm16 << 5) | (rd & 0x1F)

def arm64_ldr_x_uoff(rt: int, rn: int, byte_off: int) -> int:
    imm12 = byte_off // 8
    return (0b1111100101 << 22) | (imm12 << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)

def arm64_br(rn: int) -> int:
    return 0xD61F0000 | ((rn & 0x1F) << 5)

def add_lc_load_dylib(buf: bytearray, dylib_path: str):
    """Insert LC_LOAD_DYLIB immediately before LC_RPATH (canonical ld64 order)."""
    path_bytes = dylib_path.encode("utf-8") + b"\x00"
    cmdsize = (24 + len(path_bytes) + 7) & ~7
    cmd_buf = bytearray(cmdsize)
    struct.pack_into("<IIIIII", cmd_buf, 0, 0x0C, cmdsize, 24, 2, 0x00010000, 0x00010000)
    cmd_buf[24:24 + len(path_bytes)] = path_bytes
    ncmds, sizeofcmds = struct.unpack_from("<II", buf, 16)
    off = 32
    before = []
    after = []
    inserted = False
    for _ in range(ncmds):
        c, sz = struct.unpack_from("<II", buf, off)
        if not inserted and c in (0x8000001C, 0x26, 0x29, 0x1D):
            before.append(bytes(cmd_buf))
            inserted = True
        if inserted:
            after.append(bytes(buf[off:off + sz]))
        else:
            before.append(bytes(buf[off:off + sz]))
        off += sz
    if not inserted:
        before.append(bytes(cmd_buf))
    reordered = b"".join(before + after)
    buf[32:32 + len(reordered)] = reordered
    struct.pack_into("<II", buf, 16, ncmds + 1, len(reordered))

def patch_ios_dylib(dylib_path: str):
    with open(dylib_path, "rb") as f:
        buf = bytearray(f.read())
    _, _, _, _, ncmds, sizeofcmds, flags, _ = struct.unpack_from("<IIIIIIII", buf, 0)
    flags |= 0x02100000  # MH_APP_EXTENSION_SAFE | MH_NO_REEXPORTED_DYLIBS

    off = 32
    segs = []
    sig_dataoff = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", buf, off)
        if cmd == 0x19:
            segname = buf[off + 8:off + 24].rstrip(b"\x00").decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", buf, off + 24)
            segs.append((segname, vmaddr, vmsize, fileoff, filesize))
        elif cmd == 0x1D:
            sig_dataoff, _ = struct.unpack_from("<II", buf, off + 8)
        off += cmdsize

    off = 32
    new_cmds = bytearray()
    new_ncmds = 0
    seg_idx = 0
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", buf, off)
        chunk = bytearray(buf[off:off + cmdsize])
        if cmd == 0x0E:  # Strip LC_LOAD_DYLINKER for iOS dyld4 MH_DYLIB
            off += cmdsize
            continue
        if cmd in (0x26, 0x29):  # Strip 0-size LC_FUNCTION_STARTS / LC_DATA_IN_CODE
            _, dsize = struct.unpack_from("<II", chunk, 8)
            if dsize == 0:
                off += cmdsize
                continue
        if cmd == 0x19:  # Ensure 16KB contiguous page-aligned segments before __LINKEDIT
            segname, vmaddr, vmsize, fileoff, filesize = segs[seg_idx]
            if seg_idx + 1 < len(segs) and segname != "__LINKEDIT":
                next_vmaddr = segs[seg_idx + 1][1]
                next_fileoff = segs[seg_idx + 1][3]
                vmsize = next_vmaddr - vmaddr
                filesize = next_fileoff - fileoff
                struct.pack_into("<QQQQ", chunk, 24, vmaddr, vmsize, fileoff, filesize)
            seg_idx += 1
        elif cmd in (0x22, 0x80000022):  # LC_DYLD_INFO_ONLY: clear 0-size offsets
            for field_off in (8, 16, 24, 32, 40):
                foff, fsz = struct.unpack_from("<II", chunk, field_off)
                if fsz == 0 and foff != 0:
                    struct.pack_into("<I", chunk, field_off, 0)
        elif cmd == 0x02 and sig_dataoff is not None:  # LC_SYMTAB: align strsize to sig_dataoff
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", chunk, 8)
            if stroff > 0 and sig_dataoff >= stroff:
                struct.pack_into("<I", chunk, 20, sig_dataoff - stroff)
        elif cmd == 0x32:  # LC_BUILD_VERSION matching Sapple executable
            if len(chunk) >= 32:
                struct.pack_into("<IIIIII", chunk, 8, 2, 0x00120000, 0x001B0000, 1, 3, 0x699CD500)
            else:
                struct.pack_into("<III", chunk, 8, 2, 0x00120000, 0x001B0000)
        new_cmds.extend(chunk)
        new_ncmds += 1
        off += cmdsize
    buf[32:32 + sizeofcmds] = b"\x00" * sizeofcmds
    buf[32:32 + len(new_cmds)] = new_cmds
    struct.pack_into("<III", buf, 16, new_ncmds, len(new_cmds), flags)
    with open(dylib_path, "wb") as f:
        f.write(buf)

def patch_host(raw: bytes, cfg: dict, onecore_list: list, neural_list: list) -> bytes:
    buf = bytearray(raw)
    n_oc = len(onecore_list)
    n_nv = len(neural_list)
    neural_bytes = b"".join(encode_tuple(n, f, l) for n, f, l in neural_list)
    onecore_bytes = b"".join(encode_tuple(n, f, l) for n, f, l in onecore_list)

    dc_off = cfg["dc_file_off"]
    buf[dc_off:dc_off + len(neural_bytes)] = neural_bytes
    oc_off = dc_off + 0x3A0
    buf[oc_off:oc_off + len(onecore_bytes)] = onecore_bytes

    neural_vm = 0x100000000 + dc_off
    onecore_vm = 0x100000000 + oc_off

    cv_pc = cfg["classic_adrp_vm"]
    new_tv_vm = cfg["new_tv_vm"]
    struct.pack_into(
        "<II", buf, cv_pc - 0x100000000,
        arm64_adrp(0, cv_pc, new_tv_vm & ~0xFFF),
        arm64_add_imm(0, 0, new_tv_vm & 0xFFF),
    )

    oc_loop_pc = cfg["oc_loop_adrp_vm"]
    oc_reg = cfg["oc_loop_reg"]
    oc_cnt_reg = cfg["oc_cnt_reg"]
    oc_loop_target = onecore_vm + 0x20
    struct.pack_into(
        "<III", buf, oc_loop_pc - 0x100000000,
        arm64_adrp(oc_reg, oc_loop_pc, oc_loop_target & ~0xFFF),
        arm64_add_imm(oc_reg, oc_reg, oc_loop_target & 0xFFF),
        arm64_movz_w(oc_cnt_reg, n_oc),
    )

    oc_end_pc = cfg["oc_end_adrp_vm"]
    struct.pack_into(
        "<III", buf, oc_end_pc - 0x100000000,
        arm64_adrp(0, oc_end_pc, onecore_vm & ~0xFFF),
        arm64_add_imm(0, 0, onecore_vm & 0xFFF),
        arm64_movz_w(1, n_oc),
    )

    nv_cnt1_pc = cfg["nv_cnt1_vm"]
    nv_cnt2_pc = cfg["nv_cnt2_vm"]
    struct.pack_into("<I", buf, nv_cnt1_pc - 0x100000000, arm64_movz_w(26, n_nv))
    struct.pack_into("<I", buf, nv_cnt2_pc - 0x100000000, arm64_movz_w(1, n_nv))

    nv_loop_pc = cfg["nv_loop_adrp_vm"]
    nv_loop_target = neural_vm + 0x20
    struct.pack_into(
        "<II", buf, nv_loop_pc - 0x100000000,
        arm64_adrp(27, nv_loop_pc, nv_loop_target & ~0xFFF),
        arm64_add_imm(27, 27, nv_loop_target & 0xFFF),
    )

    nv_end_pc = cfg["nv_end_adrp_vm"]
    struct.pack_into(
        "<III", buf, nv_end_pc - 0x100000000,
        arm64_adrp(0, nv_end_pc, neural_vm & ~0xFFF),
        arm64_add_imm(0, 0, neural_vm & 0xFFF),
        arm64_movz_w(1, n_nv),
    )

    fn_table_vm = cfg["fn_table_vm"]
    for idx, fn_vm in enumerate(cfg["bridge_fns"]):
        slot_vm = fn_table_vm + idx * 8
        struct.pack_into(
            "<III", buf, fn_vm - 0x100000000,
            arm64_adrp(16, fn_vm, slot_vm & ~0xFFF),
            arm64_ldr_x_uoff(16, 16, slot_vm & 0xFFF),
            arm64_br(16),
        )

    add_lc_load_dylib(buf, "@rpath/libSappleMultilang.dylib")
    return bytes(buf)

def write_dir_entry(zout: zipfile.ZipFile, dir_path: str):
    if not dir_path.endswith("/"):
        dir_path += "/"
    zi = zipfile.ZipInfo(dir_path)
    zi.external_attr = (0o040755 << 16) | 0x10
    zi.compress_type = zipfile.ZIP_STORED
    zout.writestr(zi, b"")

def package_ipa(out_path: str, app_patched: bytes, ext_patched: bytes, dylib_bytes: bytes,
                oc_files_filter=None, nv_folders_filter=None, include_protalker=True,
                pack_id="Full", pack_desc="全言語フルパック"):
    onecore_dir = os.path.join(REPO_DIR, "OneCoreVoice")
    neural_dir = os.path.join(REPO_DIR, "NeuralVoices")
    protalker_dir = os.path.join(REPO_DIR, "ProTalkerVoice")
    ext_prefix = "Payload/Sapple.app/PlugIns/ClassicVoicesExtension.appex/"

    # Always include MSTTSLocEnUS.dat and enUS.*.dat in every pack so ClassicEngine.oneCoreDataDir
    # (which probes MSTTSLocEnUS.dat) and cvn_bridge always find the base language files.
    always_oc = {
        "msttslocenus.dat",
        "enus.address.dat",
        "enus.computer.dat",
        "enus.media.dat",
        "enus.message.dat",
        "enus.name.dat",
        "enus.phone.dat",
        "enus.query.dat",
    }

    with zipfile.ZipFile(BASE_IPA, "r") as zin, zipfile.ZipFile(
        out_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6
    ) as zout:
        for item in zin.infolist():
            fn = item.filename
            if fn.startswith(ext_prefix + "OneCoreVoice/") or fn.startswith(ext_prefix + "NeuralVoices/"):
                continue
            if fn == "Payload/Sapple.app/Sapple":
                zout.writestr(item, app_patched)
            elif fn == ext_prefix + "ClassicVoicesExtension":
                zout.writestr(item, ext_patched)
            else:
                zout.writestr(item, zin.read(fn))

        write_dir_entry(zout, "Payload/Sapple.app/Frameworks/")
        info1 = zipfile.ZipInfo("Payload/Sapple.app/Frameworks/libSappleMultilang.dylib")
        info1.external_attr = 0o100755 << 16
        info1.compress_type = zipfile.ZIP_DEFLATED
        zout.writestr(info1, dylib_bytes)

        info2 = zipfile.ZipInfo(ext_prefix + "Frameworks/libSappleMultilang.dylib")
        info2.external_attr = 0o100755 << 16
        info2.compress_type = zipfile.ZIP_DEFLATED
        zout.writestr(info2, dylib_bytes)

        write_dir_entry(zout, ext_prefix + "OneCoreVoice/")
        if pack_id:
            zm = zipfile.ZipInfo(ext_prefix + "OneCoreVoice/.pack_info")
            zm.external_attr = 0o100644 << 16
            zm.compress_type = zipfile.ZIP_STORED
            zout.writestr(zm, f"{pack_id}|{pack_desc}\n".encode("utf-8"))

        oc_count = 0
        for f in sorted(os.listdir(onecore_dir)):
            if f.startswith("."):
                continue
            fl = f.lower()
            if oc_files_filter is not None and fl not in oc_files_filter and fl not in always_oc:
                continue
            fp = os.path.join(onecore_dir, f)
            if os.path.isfile(fp):
                zi = zipfile.ZipInfo(ext_prefix + "OneCoreVoice/" + f)
                zi.external_attr = 0o100644 << 16
                zi.compress_type = zipfile.ZIP_DEFLATED
                with open(fp, "rb") as fh:
                    zout.writestr(zi, fh.read())
                oc_count += 1

        write_dir_entry(zout, ext_prefix + "NeuralVoices/")
        nv_count = 0
        mk = os.path.join(neural_dir, "model.key")
        if os.path.isfile(mk):
            zi = zipfile.ZipInfo(ext_prefix + "NeuralVoices/model.key")
            zi.external_attr = 0o100644 << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            with open(mk, "rb") as fh:
                zout.writestr(zi, fh.read())
            nv_count += 1

        wrote_jenny_voc = False
        for sub in sorted(os.listdir(neural_dir)):
            sub_path = os.path.join(neural_dir, sub)
            if not os.path.isdir(sub_path):
                continue
            if nv_folders_filter is not None and sub not in nv_folders_filter:
                continue
            write_dir_entry(zout, f"{ext_prefix}NeuralVoices/{sub}/")
            for f in sorted(os.listdir(sub_path)):
                fp = os.path.join(sub_path, f)
                if os.path.isfile(fp):
                    zi = zipfile.ZipInfo(f"{ext_prefix}NeuralVoices/{sub}/{f}")
                    zi.external_attr = 0o100644 << 16
                    zi.compress_type = zipfile.ZIP_DEFLATED
                    with open(fp, "rb") as fh:
                        zout.writestr(zi, fh.read())
                    nv_count += 1
                    if sub == "Jenny" and f == "device_vocoder_v6_streaming.bin":
                        wrote_jenny_voc = True

        # Ensure Jenny/device_vocoder_v6_streaming.bin is always present for shared vocoder linking
        if not wrote_jenny_voc:
            voc_fp = os.path.join(neural_dir, "Jenny", "device_vocoder_v6_streaming.bin")
            if os.path.isfile(voc_fp):
                write_dir_entry(zout, f"{ext_prefix}NeuralVoices/Jenny/")
                zi = zipfile.ZipInfo(f"{ext_prefix}NeuralVoices/Jenny/device_vocoder_v6_streaming.bin")
                zi.external_attr = 0o100644 << 16
                zi.compress_type = zipfile.ZIP_DEFLATED
                with open(voc_fp, "rb") as fh:
                    zout.writestr(zi, fh.read())
                nv_count += 1

        pt_count = 0
        if include_protalker and os.path.isdir(protalker_dir):
            write_dir_entry(zout, ext_prefix + "ProTalkerVoice/")
            for root, dirs, files in os.walk(protalker_dir):
                rel_dir = os.path.relpath(root, protalker_dir).replace("\\", "/")
                if rel_dir != ".":
                    write_dir_entry(zout, f"{ext_prefix}ProTalkerVoice/{rel_dir}/")
                for f in sorted(files):
                    fp = os.path.join(root, f)
                    rel_file = f if rel_dir == "." else f"{rel_dir}/{f}"
                    zi = zipfile.ZipInfo(f"{ext_prefix}ProTalkerVoice/{rel_file}")
                    zi.external_attr = 0o100644 << 16
                    zi.compress_type = zipfile.ZIP_DEFLATED
                    with open(fp, "rb") as fh:
                        zout.writestr(zi, fh.read())
                    pt_count += 1

    print(f"Packaged {out_path} ({os.path.getsize(out_path) / (1024*1024):.1f} MB): "
          f"{oc_count} OneCore, {nv_count} Neural, {pt_count} ProTalker files.")

def main():
    dylib_out = os.path.join(REPO_DIR, "libSappleMultilang.dylib")
    onecore_c = [p for p in glob.glob(os.path.join(REPO_DIR, "Engine", "onecore", "*.c")) if not p.endswith("zira_say.c")]
    sam_c = [p for p in glob.glob(os.path.join(REPO_DIR, "Engine", "sam", "*.c")) if not p.endswith("sam_say.c")]
    protalker_c = [os.path.join(REPO_DIR, "Engine", "protalker", "protalker_tts.c")]
    emu_c = [
        os.path.join(REPO_DIR, "SAPI4", "emu", "x86.c"),
        os.path.join(REPO_DIR, "SAPI4", "emu", "emu.c"),
        os.path.join(REPO_DIR, "SAPI4", "emu", "crt.c"),
        os.path.join(REPO_DIR, "SAPI4", "emu", "winapi.c"),
        os.path.join(REPO_DIR, "SAPI4", "emu", "ole.c"),
        os.path.join(REPO_DIR, "SAPI4", "emu", "registry.c"),
    ]
    bridge_c = [
        os.path.join(REPO_DIR, "Shared", "Bridge", "cv_bridge.c"),
        os.path.join(REPO_DIR, "Shared", "Bridge", "cv_resample.c"),
        os.path.join(REPO_DIR, "Shared", "Bridge", "cvo_bridge.c"),
        os.path.join(REPO_DIR, "Shared", "Bridge", "cvn_bridge.c"),
        os.path.join(REPO_DIR, "Shared", "Bridge", "sapple_multilang_init.c"),
    ]
    cmd = [
        ZIG_EXE, "cc", "-target", "aarch64-macos", "-O2", "-dynamiclib",
        "-DCV_NO_DEBUG_ENV=1",
        "-Wl,-headerpad,0x4000",
        "-Wl,-install_name,@rpath/libSappleMultilang.dylib",
        f"-I{os.path.join(REPO_DIR, 'Engine', 'onecore')}",
        f"-I{os.path.join(REPO_DIR, 'Engine', 'sam')}",
        f"-I{os.path.join(REPO_DIR, 'Engine', 'protalker')}",
        f"-I{os.path.join(REPO_DIR, 'SAPI4', 'emu')}",
        f"-I{os.path.join(REPO_DIR, 'Shared', 'Bridge')}",
        *onecore_c, *sam_c, *protalker_c, *emu_c, *bridge_c,
        "-o", dylib_out,
    ]
    subprocess.check_call(cmd)
    patch_ios_dylib(dylib_out)
    with open(dylib_out, "rb") as f:
        dylib_bytes = f.read()
    print("Compiled and patched libSappleMultilang.dylib with Zig CC (including ProTALKER 97).")

    orig_onecore = [
        ("David", False, "en-US"), ("Zira", True, "en-US"), ("Mark", False, "en-US"),
        ("Hazel", True, "en-GB"), ("George", False, "en-GB"), ("Susan", True, "en-GB"),
        ("Eva", True, "en-US"), ("Sarah", True, "en-GB"),
        ("Catherine", True, "en-AU"), ("James", False, "en-AU"),
        ("Linda", True, "en-US"), ("Richard", False, "en-US"),
        ("Matilda", True, "en-AU"),
    ]
    orig_names = {x[0] for x in orig_onecore}
    with open(os.path.join(REPO_DIR, "Engine", "onecore", "zira_tts.c"), "r", encoding="utf-8") as f:
        c_txt = f.read()
    entries = re.findall(r'\{\"([^\"]+)\",\s*\"([^\"]+)\",\s*\"([^\"]+)\",\s*\"([^\"]+)\",\s*(\d+),\s*(\d+),\s*(\d+)\}', c_txt)

    vo_lang_remap = {
        "en-CA": "en-US",
        "de-AT": "de-DE",
        "de-CH": "de-DE",
        "fr-CH": "fr-FR",
        "ar-EG": "ar-SA",
    }

    extra_onecore = []
    voice_orig_loc = {}
    for name, _, loc in orig_onecore:
        voice_orig_loc[name] = loc.lower()
    for name, prefix, loc, dat, lcid, male, neural in entries:
        voice_orig_loc[name] = loc.lower()
        if name in orig_names:
            continue
        female = (int(male) == 0)
        parts = loc.split("-")
        bcp47 = parts[0].lower() + "-" + parts[1].upper()
        bcp47 = vo_lang_remap.get(bcp47, bcp47)
        extra_onecore.append((name, female, bcp47))

    full_onecore_list = orig_onecore + extra_onecore
    full_neural_list = [
        ("Jenny", True, "en-US"), ("Aria", True, "en-US"), ("Guy", False, "en-US"),
        ("Sonia", True, "en-GB"), ("Ryan", False, "en-GB"),
        ("Neerja", True, "en-IN"), ("Prabhat", False, "en-IN"),
        ("Nanami", True, "ja-JP"), ("Keita", False, "ja-JP"),
        ("Xiaoxiao", True, "zh-CN"), ("Yunxi", False, "zh-CN"),
        ("KatjaNatural", True, "de-DE"), ("Conrad", False, "de-DE"),
        ("Elvira", True, "es-ES"), ("Alvaro", False, "es-ES"),
        ("Dalia", True, "es-MX"), ("Jorge", False, "es-MX"),
        ("Denise", True, "fr-FR"), ("Henri", False, "fr-FR"),
        ("SunHi", True, "ko-KR"), ("InJoon", False, "ko-KR"),
        ("Francisca", True, "pt-BR"), ("Antonio", False, "pt-BR"),
    ]

    ext_cfg = {
        "dc_file_off": 0x12A000,
        "fn_table_vm": 0x100262600,
        "new_tv_vm":   0x100262700,
        "classic_adrp_vm": 0x1000E4D5C,
        "oc_loop_adrp_vm": 0x1000E50A8,
        "oc_loop_reg": 27,
        "oc_cnt_reg":  28,
        "oc_end_adrp_vm":  0x1000E51EC,
        "nv_cnt1_vm":      0x1000E6574,
        "nv_cnt2_vm":      0x1000E6580,
        "nv_loop_adrp_vm": 0x1000E6590,
        "nv_end_adrp_vm":  0x1000E66C8,
        "bridge_fns": [
            0x10002B6A0, 0x10002B89C, 0x10002B8D8, 0x10002B92C,
            0x10002A504, 0x10002AC6C, 0x10002AC9C, 0x10002A468,
        ],
    }

    app_cfg = {
        "dc_file_off": 0x136A00,
        "fn_table_vm": 0x10026EC00,
        "new_tv_vm":   0x10026ED00,
        "classic_adrp_vm": 0x1000F081C,
        "oc_loop_adrp_vm": 0x1000F0B68,
        "oc_loop_reg": 26,
        "oc_cnt_reg":  27,
        "oc_end_adrp_vm":  0x1000F0CA4,
        "nv_cnt1_vm":      0x1000F202C,
        "nv_cnt2_vm":      0x1000F2038,
        "nv_loop_adrp_vm": 0x1000F2048,
        "nv_end_adrp_vm":  0x1000F2180,
        "bridge_fns": [
            0x10002B704, 0x10002B900, 0x10002B93C, 0x10002B990,
            0x10002A568, 0x10002ACD0, 0x10002AD00, 0x10002A4CC,
        ],
    }

    with zipfile.ZipFile(BASE_IPA, "r") as z:
        app_raw = z.read("Payload/Sapple.app/Sapple")
        ext_raw = z.read("Payload/Sapple.app/PlugIns/ClassicVoicesExtension.appex/ClassicVoicesExtension")

    # Map every file in OneCoreVoice to its locale
    oc_dir = os.path.join(REPO_DIR, "OneCoreVoice")
    file_to_loc = {}
    for name, prefix, loc, dat, lcid, male, neural in entries:
        loc_norm = loc.lower()
        file_to_loc[dat.lower()] = loc_norm
        for f in os.listdir(oc_dir):
            if f.lower().startswith(prefix.lower() + "."):
                file_to_loc[f.lower()] = loc_norm
    for f in os.listdir(oc_dir):
        fl = f.lower()
        if fl not in file_to_loc:
            m = re.match(r"^([a-z]{2})([a-z]{2})\.", fl)
            if m:
                file_to_loc[fl] = m.group(1) + "-" + m.group(2)

    def oc_files_for_locs(locs: set) -> set:
        return {f.lower() for f in os.listdir(oc_dir) if file_to_loc.get(f.lower()) in locs}

    # Pack 1: Japanese + English + ProTalker + Classic Voices
    pack1_locs = {"ja-jp", "en-us", "en-gb", "en-au", "en-ca", "en-ie", "en-in"}
    pack1_nv = {"Nanami", "Keita", "ja-JP", "Jenny", "Aria", "Guy", "Sonia", "Ryan", "en-GB", "Neerja", "Prabhat", "en-IN"}

    # Pack 2: East / South / Southeast Asia
    pack2_locs = {"zh-cn", "zh-tw", "zh-hk", "ko-kr", "hi-in", "ta-in", "th-th", "vi-vn", "id-id", "ms-my"}
    pack2_nv = {"Xiaoxiao", "Yunxi", "zh-CN", "SunHi", "InJoon", "ko-KR"}

    # Pack 3: Western / Southern Europe & Latin America
    pack3_locs = {"de-de", "de-at", "de-ch", "fr-fr", "fr-ca", "fr-ch", "es-es", "es-mx", "it-it", "pt-br", "pt-pt", "nl-nl", "nl-be", "ca-es"}
    pack3_nv = {"Katja", "Conrad", "de-DE", "Denise", "Henri", "fr-FR", "Elvira", "Alvaro", "es-ES", "Dalia", "Jorge", "es-MX", "Francisca", "Antonio", "pt-BR"}

    # Pack 4: Northern / Eastern Europe & Middle East
    used_locs = pack1_locs | pack2_locs | pack3_locs
    pack4_locs = {file_to_loc[f.lower()] for f in os.listdir(oc_dir) if f.lower() in file_to_loc and file_to_loc[f.lower()] not in used_locs}
    pack4_nv = set()

    def build_lists_for_locs(locs: set):
        oc_list = [t for t in full_onecore_list if voice_orig_loc.get(t[0]) in locs]
        nv_list = [t for t in full_neural_list if t[2].lower() in locs]
        return oc_list, nv_list

    # Cumulative voice lists per pack so installing Pack 1 alone has 0 missing voices,
    # and installing Pack 1 -> 2 -> 3 -> 4 progressively unlocks each pack's voices.
    oc_p1, nv_p1 = build_lists_for_locs(pack1_locs)
    oc_p2, nv_p2 = build_lists_for_locs(pack1_locs | pack2_locs)
    oc_p3, nv_p3 = build_lists_for_locs(pack1_locs | pack2_locs | pack3_locs)

    app_p1 = patch_host(app_raw, app_cfg, oc_p1, nv_p1)
    ext_p1 = patch_host(ext_raw, ext_cfg, oc_p1, nv_p1)

    app_p2 = patch_host(app_raw, app_cfg, oc_p2, nv_p2)
    ext_p2 = patch_host(ext_raw, ext_cfg, oc_p2, nv_p2)

    app_p3 = patch_host(app_raw, app_cfg, oc_p3, nv_p3)
    ext_p3 = patch_host(ext_raw, ext_cfg, oc_p3, nv_p3)

    app_full = patch_host(app_raw, app_cfg, full_onecore_list, full_neural_list)
    ext_full = patch_host(ext_raw, ext_cfg, full_onecore_list, full_neural_list)

    package_ipa(OUT_PACK1_IPA, app_p1, ext_p1, dylib_bytes,
                oc_files_for_locs(pack1_locs), pack1_nv, include_protalker=True,
                pack_id="LangPack1_JA_EN", pack_desc="言語パック1（日本語・英語・ProTalker）")

    package_ipa(OUT_PACK2_IPA, app_p2, ext_p2, dylib_bytes,
                oc_files_for_locs(pack2_locs), pack2_nv, include_protalker=True,
                pack_id="LangPack2_EastAsia", pack_desc="言語パック2（アジア・太平洋言語）")

    package_ipa(OUT_PACK3_IPA, app_p3, ext_p3, dylib_bytes,
                oc_files_for_locs(pack3_locs), pack3_nv, include_protalker=True,
                pack_id="LangPack3_WestEurope", pack_desc="言語パック3（西欧・南欧・中南米言語）")

    package_ipa(OUT_PACK4_IPA, app_full, ext_full, dylib_bytes,
                oc_files_for_locs(pack4_locs), pack4_nv, include_protalker=True,
                pack_id="LangPack4_EastEurope_ME", pack_desc="言語パック4（北欧・東欧・中東言語）")

    shutil.copy2(OUT_PACK1_IPA, OUT_IPA)

    # Full single-file edition (all 48 locales + ProTalker)
    package_ipa(OUT_FULL_IPA, app_full, ext_full, dylib_bytes,
                None, None, include_protalker=True,
                pack_id="Full_AllVoices", pack_desc="全48言語・ProTalkerフルパック")

    stale_names = ("Sapple-Part1.ipa", "Sapple-Part2.ipa", "Sapple-JaEnVoices.ipa", "Sapple-AllVoices.ipa")
    for dst_dir in (ICLOUD_DIR, ICLOUD_DL_DIR):
        if os.path.isdir(dst_dir):
            for stale in stale_names:
                sp = os.path.join(dst_dir, stale)
                if os.path.isfile(sp):
                    try:
                        os.remove(sp)
                    except OSError:
                        pass
            for src_ipa in (OUT_PACK1_IPA, OUT_PACK2_IPA, OUT_PACK3_IPA, OUT_PACK4_IPA):
                shutil.copy2(src_ipa, os.path.join(dst_dir, os.path.basename(src_ipa)))
            print(f"Synced clean Language Pack IPAs (1-4) to {dst_dir}")

if __name__ == "__main__":
    main()
