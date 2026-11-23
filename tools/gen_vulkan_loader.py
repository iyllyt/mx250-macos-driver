#!/usr/bin/env python3
"""
Generates a complete, zero-overhead macOS Vulkan 1.3 ICD Loader (libvulkan.1.dylib)
from Mesa's bundled Khronos vk.xml registry.

Automatically discovers ICD JSON manifests in standard macOS Vulkan paths:
  1. $VK_DRIVER_FILES / $VK_ICD_FILENAMES
  2. $HOME/.local/share/vulkan/icd.d/*.json
  3. /Library/Application Support/Vulkan/icd.d/*.json
  4. /usr/local/share/vulkan/icd.d/*.json
  5. /etc/vulkan/icd.d/*.json
"""

import os
import sys
import xml.etree.ElementTree as ET


ALLOWED_PLATFORMS = {None, "", "macos", "metal"}


def parse_vk_xml(xml_path: str):
    tree = ET.parse(xml_path)
    root = tree.getroot()

    # Map command name -> platform if restricted to a platform extension
    cmd_platform = {}
    supported_cmds = set()

    # Core features 1.0 .. 1.3
    for feat in root.findall("./feature"):
        api = feat.attrib.get("api", "vulkan")
        if "vulkan" not in api.split(","):
            continue
        num = feat.attrib.get("number", "")
        if num in ("1.0", "1.1", "1.2", "1.3"):
            for req in feat.findall("./require/command"):
                supported_cmds.add(req.attrib["name"])

    # Extensions
    for ext in root.findall("./extensions/extension"):
        supported = ext.attrib.get("supported", "")
        if "vulkan" not in supported.split(","):
            continue
        plat = ext.attrib.get("platform", None)
        if plat not in ALLOWED_PLATFORMS:
            for req in ext.findall("./require/command"):
                cmd_platform.setdefault(req.attrib["name"], plat)
            continue
        # Skip video-only beta headers that need special codec headers
        ext_name = ext.attrib.get("name", "")
        if "_video_" in ext_name:
            continue
        for req in ext.findall("./require/command"):
            cname = req.attrib["name"]
            supported_cmds.add(cname)
            cmd_platform[cname] = plat

    # Extract command prototypes & aliases
    raw_cmds = {}
    aliases = {}
    for cmd in root.findall("./commands/command"):
        api = cmd.attrib.get("api", "vulkan")
        if "vulkan" not in api.split(","):
            continue
        if "alias" in cmd.attrib and "name" in cmd.attrib:
            aliases[cmd.attrib["name"]] = cmd.attrib["alias"]
            continue
        proto = cmd.find("./proto")
        if proto is None:
            continue
        cname = proto.find("./name").text.strip()
        ret_type = "".join(proto.itertext()).rsplit(cname, 1)[0].strip()
        params = []
        for p in cmd.findall("./param"):
            papi = p.attrib.get("api", "vulkan")
            if "vulkan" not in papi.split(","):
                continue
            pname = p.find("./name").text.strip()
            ptype_el = p.find("./type")
            ptype = ptype_el.text.strip() if ptype_el is not None else ""
            full_decl = " ".join("".join(p.itertext()).split())
            params.append((ptype, pname, full_decl))
        raw_cmds[cname] = (ret_type, params)

    # Resolve exported command list
    exported = []
    for cname in sorted(supported_cmds):
        target = cname
        while target in aliases:
            target = aliases[target]
        if target not in raw_cmds:
            continue
        # Skip beta/provisional types not enabled without VK_ENABLE_BETA_EXTENSIONS
        ret_type, params = raw_cmds[target]
        full_sig = ret_type + " " + " ".join(d for _, _, d in params)
        if any(tok in full_sig for tok in ("AMDX", "NVX", "QNX", "FUCHSIA", "GGP", "ANDROID", "WIN32", "D3D12")):
            continue
        exported.append((cname, ret_type, params))

    return exported


def classify_cmd(cname: str, params: list) -> str:
    if cname in (
        "vkGetInstanceProcAddr",
        "vkGetDeviceProcAddr",
        "vkCreateInstance",
        "vkDestroyInstance",
        "vkCreateDevice",
        "vkDestroyDevice",
        "vkEnumerateInstanceLayerProperties",
        "vkEnumerateInstanceExtensionProperties",
        "vkEnumerateInstanceVersion",
    ):
        return "SPECIAL"
    if not params:
        return "INST"
    first_type = params[0][0]
    if first_type in ("VkDevice", "VkQueue", "VkCommandBuffer"):
        return "DEV"
    return "INST"


def emit_loader_c(exported, out_c: str):
    lines = []
    a = lines.append
    a("/* Auto-generated macOS Vulkan 1.3 ICD Loader (libvulkan.1.dylib) */")
    a("#define VK_NO_PROTOTYPES")
    a("#define VK_USE_PLATFORM_MACOS_MVK")
    a("#define VK_USE_PLATFORM_METAL_EXT")
    a("#include <vulkan/vulkan.h>")
    a("#include <dirent.h>")
    a("#include <dlfcn.h>")
    a("#include <limits.h>")
    a("#include <pthread.h>")
    a("#include <stdio.h>")
    a("#include <stdlib.h>")
    a("#include <string.h>")
    a("")
    a("typedef VkResult (VKAPI_PTR *PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(uint32_t *pSupportedVersion);")
    a("")
    a("static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;")
    a("static void *g_icd_dl = NULL;")
    a("static PFN_vkGetInstanceProcAddr g_icd_gipa = NULL;")
    a("static VkInstance g_last_instance = VK_NULL_HANDLE;")
    a("static VkDevice g_last_device = VK_NULL_HANDLE;")
    a("static PFN_vkGetDeviceProcAddr g_icd_gdpa = NULL;")
    a("")
    a("static int extract_json_library_path(const char *json_file, char *out_path, size_t cap) {")
    a("    FILE *fp = fopen(json_file, \"rb\");")
    a("    if (!fp) return -1;")
    a("    char buf[8192];")
    a("    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);")
    a("    fclose(fp);")
    a("    buf[n] = '\\0';")
    a("    const char *k = strstr(buf, \"\\\"library_path\\\"\");")
    a("    if (!k) return -1;")
    a("    const char *colon = strchr(k + 14, ':');")
    a("    if (!colon) return -1;")
    a("    const char *q1 = strchr(colon + 1, '\"');")
    a("    if (!q1) return -1;")
    a("    const char *q2 = strchr(q1 + 1, '\"');")
    a("    if (!q2 || (size_t)(q2 - (q1 + 1)) >= cap) return -1;")
    a("    char raw[PATH_MAX];")
    a("    size_t len = (size_t)(q2 - (q1 + 1));")
    a("    memcpy(raw, q1 + 1, len);")
    a("    raw[len] = '\\0';")
    a("    if (raw[0] == '/') {")
    a("        snprintf(out_path, cap, \"%s\", raw);")
    a("    } else {")
    a("        const char *slash = strrchr(json_file, '/');")
    a("        if (slash) {")
    a("            size_t dlen = (size_t)(slash - json_file);")
    a("            snprintf(out_path, cap, \"%.*s/%s\", (int)dlen, json_file, raw);")
    a("        } else {")
    a("            snprintf(out_path, cap, \"%s\", raw);")
    a("        }")
    a("    }")
    a("    return 0;")
    a("}")
    a("")
    a("static int try_load_icd_json(const char *json_path) {")
    a("    char dylib_path[PATH_MAX];")
    a("    if (extract_json_library_path(json_path, dylib_path, sizeof(dylib_path)) != 0)")
    a("        return -1;")
    a("    void *dl = dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL);")
    a("    if (!dl) return -1;")
    a("    PFN_vk_icdNegotiateLoaderICDInterfaceVersion neg =")
    a("        (PFN_vk_icdNegotiateLoaderICDInterfaceVersion)dlsym(dl, \"vk_icdNegotiateLoaderICDInterfaceVersion\");")
    a("    if (neg) { uint32_t ver = 5; (void)neg(&ver); }")
    a("    PFN_vkGetInstanceProcAddr gipa =")
    a("        (PFN_vkGetInstanceProcAddr)dlsym(dl, \"vk_icdGetInstanceProcAddr\");")
    a("    if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(dl, \"vkGetInstanceProcAddr\");")
    a("    if (!gipa) { dlclose(dl); return -1; }")
    a("    g_icd_dl = dl;")
    a("    g_icd_gipa = gipa;")
    a("    return 0;")
    a("}")
    a("")
    a("static int scan_icd_dir(const char *dir_path) {")
    a("    DIR *d = opendir(dir_path);")
    a("    if (!d) return -1;")
    a("    struct dirent *ent;")
    a("    while ((ent = readdir(d)) != NULL) {")
    a("        size_t l = strlen(ent->d_name);")
    a("        if (l > 5 && strcmp(ent->d_name + l - 5, \".json\") == 0) {")
    a("            char full[PATH_MAX];")
    a("            snprintf(full, sizeof(full), \"%s/%s\", dir_path, ent->d_name);")
    a("            if (try_load_icd_json(full) == 0) { closedir(d); return 0; }")
    a("        }")
    a("    }")
    a("    closedir(d);")
    a("    return -1;")
    a("}")
    a("")
    a("static void loader_init_once(void) {")
    a("    const char *env = getenv(\"VK_DRIVER_FILES\");")
    a("    if (!env || !env[0]) env = getenv(\"VK_ICD_FILENAMES\");")
    a("    if (env && env[0]) {")
    a("        char copy[4096];")
    a("        snprintf(copy, sizeof(copy), \"%s\", env);")
    a("        char *save = NULL;")
    a("        for (char *tok = strtok_r(copy, \":\", &save); tok; tok = strtok_r(NULL, \":\", &save)) {")
    a("            if (try_load_icd_json(tok) == 0 || scan_icd_dir(tok) == 0) return;")
    a("        }")
    a("    }")
    a("    const char *home = getenv(\"HOME\");")
    a("    if (home && home[0]) {")
    a("        char user_icd[PATH_MAX];")
    a("        snprintf(user_icd, sizeof(user_icd), \"%s/.local/share/vulkan/icd.d\", home);")
    a("        if (scan_icd_dir(user_icd) == 0) return;")
    a("    }")
    a("    const char *sys_dirs[] = {")
    a("        \"/Library/Application Support/Vulkan/icd.d\",")
    a("        \"/usr/local/share/vulkan/icd.d\",")
    a("        \"/etc/vulkan/icd.d\",")
    a("    };")
    a("    for (size_t i = 0; i < sizeof(sys_dirs) / sizeof(sys_dirs[0]); i++) {")
    a("        if (scan_icd_dir(sys_dirs[i]) == 0) return;")
    a("    }")
    a("    /* Direct fallback to installed NVIDIA GP108 driver location */")
    a("    (void)try_load_icd_json(\"/Library/Application Support/NVIDIA-GP108/icd.d/nvidia_mx250_icd.json\");")
    a("}")
    a("")
    a("static inline void ensure_loader(void) { pthread_once(&g_init_once, loader_init_once); }")
    a("")
    a("#define EXPORT_VK __attribute__((visibility(\"default\"))) VKAPI_ATTR")
    a("")

    # Special functions
    a("EXPORT_VK VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount, VkLayerProperties *pProperties) {")
    a("    (void)pProperties;")
    a("    if (pPropertyCount) *pPropertyCount = 0;")
    a("    return VK_SUCCESS;")
    a("}")
    a("")
    a("EXPORT_VK VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *pApiVersion) {")
    a("    ensure_loader();")
    a("    if (!g_icd_gipa) return VK_ERROR_INCOMPATIBLE_DRIVER;")
    a("    PFN_vkEnumerateInstanceVersion fn = (PFN_vkEnumerateInstanceVersion)g_icd_gipa(VK_NULL_HANDLE, \"vkEnumerateInstanceVersion\");")
    a("    if (!fn) { if (pApiVersion) *pApiVersion = VK_API_VERSION_1_0; return VK_SUCCESS; }")
    a("    return fn(pApiVersion);")
    a("}")
    a("")
    a("EXPORT_VK VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {")
    a("    ensure_loader();")
    a("    if (!g_icd_gipa) return VK_ERROR_INCOMPATIBLE_DRIVER;")
    a("    PFN_vkEnumerateInstanceExtensionProperties fn = (PFN_vkEnumerateInstanceExtensionProperties)g_icd_gipa(VK_NULL_HANDLE, \"vkEnumerateInstanceExtensionProperties\");")
    a("    return fn ? fn(pLayerName, pPropertyCount, pProperties) : VK_ERROR_INCOMPATIBLE_DRIVER;")
    a("}")
    a("")
    a("EXPORT_VK VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {")
    a("    ensure_loader();")
    a("    if (!g_icd_gipa) return VK_ERROR_INCOMPATIBLE_DRIVER;")
    a("    PFN_vkCreateInstance fn = (PFN_vkCreateInstance)g_icd_gipa(VK_NULL_HANDLE, \"vkCreateInstance\");")
    a("    if (!fn) return VK_ERROR_INCOMPATIBLE_DRIVER;")
    a("    VkInstanceCreateInfo clean = *pCreateInfo;")
    a("    clean.enabledLayerCount = 0;")
    a("    clean.ppEnabledLayerNames = NULL;")
    a("    VkResult res = fn(&clean, pAllocator, pInstance);")
    a("    if (res == VK_SUCCESS && pInstance) g_last_instance = *pInstance;")
    a("    return res;")
    a("}")
    a("")
    a("EXPORT_VK void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {")
    a("    if (!instance || !g_icd_gipa) return;")
    a("    PFN_vkDestroyInstance fn = (PFN_vkDestroyInstance)g_icd_gipa(instance, \"vkDestroyInstance\");")
    a("    if (fn) fn(instance, pAllocator);")
    a("    if (g_last_instance == instance) g_last_instance = VK_NULL_HANDLE;")
    a("}")
    a("")
    a("EXPORT_VK VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {")
    a("    ensure_loader();")
    a("    if (!g_icd_gipa) return VK_ERROR_INITIALIZATION_FAILED;")
    a("    PFN_vkCreateDevice fn = (PFN_vkCreateDevice)g_icd_gipa(g_last_instance, \"vkCreateDevice\");")
    a("    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;")
    a("    VkResult res = fn(physicalDevice, pCreateInfo, pAllocator, pDevice);")
    a("    if (res == VK_SUCCESS && pDevice) {")
    a("        g_last_device = *pDevice;")
    a("        g_icd_gdpa = (PFN_vkGetDeviceProcAddr)g_icd_gipa(g_last_instance, \"vkGetDeviceProcAddr\");")
    a("    }")
    a("    return res;")
    a("}")
    a("")
    a("EXPORT_VK void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {")
    a("    if (!device || !g_icd_gdpa) return;")
    a("    PFN_vkDestroyDevice fn = (PFN_vkDestroyDevice)g_icd_gdpa(device, \"vkDestroyDevice\");")
    a("    if (fn) fn(device, pAllocator);")
    a("    if (g_last_device == device) g_last_device = VK_NULL_HANDLE;")
    a("}")
    a("")

    # Generate all other exported Vulkan entrypoints
    normal_cmds = []
    for cname, ret_type, params in exported:
        kind = classify_cmd(cname, params)
        if kind == "SPECIAL":
            continue
        normal_cmds.append((cname, ret_type, params, kind))
        param_sig = ", ".join(d for _, _, d in params) if params else "void"
        arg_names = ", ".join(n for _, n, _ in params)
        a(f"EXPORT_VK {ret_type} VKAPI_CALL {cname}({param_sig}) {{")
        a(f"    static PFN_{cname} s_fn = NULL;")
        a("    if (!s_fn) {")
        a("        ensure_loader();")
        if kind == "DEV":
            first_t, first_n, _ = params[0]
            dev_expr = first_n if first_t == "VkDevice" else "g_last_device"
            a(f"        if (g_icd_gdpa && {dev_expr}) s_fn = (PFN_{cname})g_icd_gdpa({dev_expr}, \"{cname}\");")
            a(f"        if (!s_fn && g_icd_gipa) s_fn = (PFN_{cname})g_icd_gipa(g_last_instance, \"{cname}\");")
        else:
            inst_expr = params[0][1] if (params and params[0][0] == "VkInstance") else "g_last_instance"
            a(f"        if (g_icd_gipa) s_fn = (PFN_{cname})g_icd_gipa({inst_expr}, \"{cname}\");")
        a("    }")
        if ret_type == "void":
            a(f"    if (s_fn) s_fn({arg_names});")
        elif ret_type == "VkResult":
            a(f"    return s_fn ? s_fn({arg_names}) : VK_ERROR_EXTENSION_NOT_PRESENT;")
        else:
            a(f"    return s_fn ? s_fn({arg_names}) : ({ret_type})0;")
        a("}")
        a("")

    # Loader lookup table for vkGetInstanceProcAddr / vkGetDeviceProcAddr
    a("EXPORT_VK PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName);")
    a("EXPORT_VK PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName);")
    a("typedef struct { const char *name; PFN_vkVoidFunction fn; } LoaderSym;")
    a("static const LoaderSym g_loader_syms[] = {")
    for sname in (
        "vkGetInstanceProcAddr",
        "vkGetDeviceProcAddr",
        "vkCreateInstance",
        "vkDestroyInstance",
        "vkCreateDevice",
        "vkDestroyDevice",
        "vkEnumerateInstanceLayerProperties",
        "vkEnumerateInstanceExtensionProperties",
        "vkEnumerateInstanceVersion",
    ):
        a(f"    {{ \"{sname}\", (PFN_vkVoidFunction){sname} }},")
    for cname, _, _, _ in normal_cmds:
        a(f"    {{ \"{cname}\", (PFN_vkVoidFunction){cname} }},")
    a("};")
    a("")
    a("EXPORT_VK PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName) {")
    a("    if (!pName) return NULL;")
    a("    ensure_loader();")
    a("    if (strcmp(pName, \"vkGetDeviceProcAddr\") == 0) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;")
    a("    if (strcmp(pName, \"vkDestroyDevice\") == 0) return (PFN_vkVoidFunction)vkDestroyDevice;")
    a("    if (g_icd_gdpa && device) return g_icd_gdpa(device, pName);")
    a("    return NULL;")
    a("}")
    a("")
    a("EXPORT_VK PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName) {")
    a("    if (!pName) return NULL;")
    a("    ensure_loader();")
    a("    if (!instance) {")
    a("        if (strcmp(pName, \"vkGetInstanceProcAddr\") == 0) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;")
    a("        if (strcmp(pName, \"vkCreateInstance\") == 0) return (PFN_vkVoidFunction)vkCreateInstance;")
    a("        if (strcmp(pName, \"vkEnumerateInstanceLayerProperties\") == 0) return (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties;")
    a("        if (strcmp(pName, \"vkEnumerateInstanceExtensionProperties\") == 0) return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;")
    a("        if (strcmp(pName, \"vkEnumerateInstanceVersion\") == 0) return (PFN_vkVoidFunction)vkEnumerateInstanceVersion;")
    a("        return NULL;")
    a("    }")
    a("    /* Check if ICD supports this entrypoint before returning loader trampoline */")
    a("    PFN_vkVoidFunction icd_fn = g_icd_gipa ? g_icd_gipa(instance, pName) : NULL;")
    a("    for (size_t i = 0; i < sizeof(g_loader_syms) / sizeof(g_loader_syms[0]); i++) {")
    a("        if (strcmp(g_loader_syms[i].name, pName) == 0)")
    a("            return (icd_fn || strncmp(pName, \"vkCreate\", 8) == 0 || strncmp(pName, \"vkDestroy\", 9) == 0) ? g_loader_syms[i].fn : NULL;")
    a("    }")
    a("    return icd_fn;")
    a("}")

    with open(out_c, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Generated {out_c} with {len(exported)} exported Vulkan entrypoints.")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    xml_path = os.path.join(root, "third_party/mesa_src/src/vulkan/registry/vk.xml")
    out_c = os.path.join(root, "build/vulkan_loader_macos.c")
    exported = parse_vk_xml(xml_path)
    emit_loader_c(exported, out_c)


if __name__ == "__main__":
    main()
