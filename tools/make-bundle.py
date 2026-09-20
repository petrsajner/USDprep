# Builds the self-contained folder that gets shipped: usdtweak with the
# Prep panel, usdcut, every DLL they need and USD's plugins - nothing from
# conda, no PATH, no admin rights. What goes in is decided by walking the
# import tables (dumpbin), not by a hand-kept list.
#
#   python tools/make-bundle.py [--out dist]
#
# Layout (USD looks for its plugins relative to its own DLLs):
#   usdprep-<version>/bin/          usdtweak.exe, usdcut.exe, *.dll
#   usdprep-<version>/bin/usd/      USD core plugin resources
#   usdprep-<version>/plugin/usd/   hdStorm, hio*, sdr* plugins
#   usdprep-<version>/licenses/     the licences of what is bundled
import argparse, glob, json, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = os.path.join(ROOT, "third_party", "usdtools", ".pixi", "envs", "default")
USDTWEAK = os.path.join(ROOT, "third_party", "usdtweak", "build-conda", "RelWithDebInfo", "usdtweak.exe")
USDCUT = os.path.join(ROOT, "build", "src", "cli", "RelWithDebInfo", "usdcut.exe")
SEARCH = [os.path.join(ENV, "Library", "bin"), ENV, os.path.join(ENV, "DLLs")]


def find_dumpbin():
    hits = glob.glob(r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe")
    hits += glob.glob(r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe")
    if not hits:
        sys.exit("dumpbin.exe not found (Visual Studio build tools)")
    return sorted(hits)[-1]


DUMPBIN = find_dumpbin()


def imports_of(path):
    out = subprocess.run([DUMPBIN, "/nologo", "/dependents", path], capture_output=True, text=True).stdout
    return [m.group(1) for m in re.finditer(r"^\s+(\S+\.dll)\s*$", out, re.I | re.M)]


def resolve(name):
    for folder in SEARCH:
        candidate = os.path.join(folder, name)
        if os.path.exists(candidate):
            return candidate
    return None


def version():
    text = open(os.path.join(ROOT, "src", "core", "include", "usdprep", "Version.h")).read()
    m = re.search(r'USDPREP_VERSION_STRING\s+"([^"]+)"', text)
    return m.group(1) if m else "dev"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    parser.add_argument("--installer", action="store_true", help="also compile the Inno Setup installer")
    args = parser.parse_args()
    for needed in (USDTWEAK, USDCUT):
        if not os.path.exists(needed):
            sys.exit("not built yet: " + needed)

    bundle = os.path.join(args.out, "usdprep-" + version())
    shutil.rmtree(bundle, ignore_errors=True)
    bin_dir = os.path.join(bundle, "bin")
    os.makedirs(bin_dir)

    # USD loads its libraries as plugins, by name, at run time - the import
    # tables do not know about them, so every usd_*.dll and plugin is a root.
    plugin_src = os.path.join(ENV, "Library", "plugin", "usd")
    roots = [USDTWEAK, USDCUT]
    roots += glob.glob(os.path.join(ENV, "Library", "bin", "usd_*.dll"))
    roots += glob.glob(os.path.join(plugin_src, "*.dll"))

    seen, queue, system = {}, list(roots), set()
    while queue:
        path = queue.pop()
        key = os.path.basename(path).lower()
        if key in seen:
            continue
        seen[key] = path
        for name in imports_of(path):
            if name.lower() in seen:
                continue
            found = resolve(name)
            if found:
                queue.append(found)
            else:
                system.add(name.lower())

    plugin_dlls = {os.path.basename(p).lower() for p in glob.glob(os.path.join(plugin_src, "*.dll"))}
    for key, path in seen.items():
        if key not in plugin_dlls:
            shutil.copy2(path, bin_dir)
    shutil.copytree(os.path.join(ENV, "Library", "bin", "usd"), os.path.join(bin_dir, "usd"))
    shutil.copytree(plugin_src, os.path.join(bundle, "plugin", "usd"),
                    ignore=shutil.ignore_patterns("*.lib", "*.pdb"))

    # Licences: every conda package a bundled file came from, with the
    # licence texts from its package cache, plus a notice that lists them.
    licenses = os.path.join(bundle, "licenses")
    os.makedirs(licenses)
    bundled = set(seen) | {f.lower() for f in os.listdir(os.path.join(bundle, "plugin", "usd"))}
    notices = []
    for meta in sorted(glob.glob(os.path.join(ENV, "conda-meta", "*.json"))):
        info = json.load(open(meta, encoding="utf-8"))
        if not any(os.path.basename(f).lower() in bundled for f in info.get("files", []) if f.lower().endswith(".dll")):
            continue
        notices.append("%s %s - %s" % (info["name"], info["version"], info.get("license", "see package")))
        texts = os.path.join(info.get("extracted_package_dir", ""), "info", "licenses")
        if os.path.isdir(texts):
            shutil.copytree(texts, os.path.join(licenses, info["name"]), dirs_exist_ok=True)
    for base, label in ((os.path.join(ROOT, "third_party", "usdtweak"), "usdtweak"),
                        (os.path.join(ROOT, "src", "third_party", "meshoptimizer"), "meshoptimizer")):
        for name in ("LICENSE", "LICENSE.md", "LICENSE.txt"):
            if os.path.exists(os.path.join(base, name)):
                os.makedirs(os.path.join(licenses, label), exist_ok=True)
                shutil.copy2(os.path.join(base, name), os.path.join(licenses, label, name))
                notices.append(label + " - see licenses/" + label)
    with open(os.path.join(bundle, "THIRD_PARTY_NOTICES.txt"), "w", encoding="utf-8") as f:
        f.write("usdprep bundles these components; their licence texts are in the licenses folder.\n\n")
        f.write("\n".join(notices) + "\n")

    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, files in os.walk(bundle) for f in files)
    unexpected = sorted(n for n in system if not os.path.exists(os.path.join(r"C:\Windows\System32", n))
                        and not n.startswith(("api-ms-", "ext-ms-")))
    print("bundle :", bundle)
    print("files  : %d DLL/EXE, %.0f MB in total" % (len(seen), size / 1e6))
    print("system : %d DLLs left to Windows" % len(system))
    if unexpected:
        print("MISSING (neither in the env nor in System32):", ", ".join(unexpected))
        sys.exit(1)

    if args.installer:
        iscc = glob.glob(os.path.expandvars(r"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"))
        iscc += glob.glob(r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe")
        if not iscc:
            sys.exit("Inno Setup 6 (ISCC.exe) not found")
        subprocess.run([iscc[0], "/Q", "/DAppVersion=" + version(), "/DBundleDir=" + bundle, "/DOutDir=" + args.out,
                        os.path.join(ROOT, "installer", "usdprep.iss")], check=True)
        setup = os.path.join(args.out, "usdprep-%s-setup.exe" % version())
        print("setup  : %s (%.0f MB)" % (setup, os.path.getsize(setup) / 1e6))


if __name__ == "__main__":
    main()
