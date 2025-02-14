// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#include "src/core/base/base.hh"
#include "src/core/wrap/json.hh"
#include "build.hh"
#include "locate.hh"
#include "vendor/pugixml/src/pugixml.hpp"

namespace RG {

#ifdef _WIN32
    #define MAX_COMMAND_LEN 4096
#else
    #define MAX_COMMAND_LEN 32768
#endif

template <typename T>
static bool AssembleResourceFile(const pugi::xml_document *doc, const char *icon_filename, T *out_buf)
{
    class StaticWriter: public pugi::xml_writer {
        T *out_buf;
        bool error = false;

    public:
        StaticWriter(T *out_buf) : out_buf(out_buf) {}

        bool IsValid() const { return !error; }

        void Append(Span<const char> str)
        {
            error |= (str.len > out_buf->Available());
            if (error) [[unlikely]]
                return;

            out_buf->Append(str);
        }

        void write(const void *buf, size_t len) override
        {
            for (Size i = 0; i < (Size)len; i++) {
                int c = ((const uint8_t *)buf)[i];

                switch (c) {
                    case '\"': { Append("\"\""); } break;
                    case '\t':
                    case '\r': {} break;
                    case '\n': { Append("\\n\",\n\t\""); } break;

                    default: {
                        if (c < 32 || c >= 128) {
                            error |= (out_buf->Available() < 4);
                            if (error) [[unlikely]]
                                return;

                            Fmt(out_buf->TakeAvailable(), "\\x%1", FmtHex(c).Pad0(-2));
                            out_buf->len += 4;
                        } else {
                            char ch = (char)c;
                            Append(ch);
                        }
                    } break;
                }
            }
        }
    };

    StaticWriter writer(out_buf);
    writer.Append("#include <winuser.h>\n\n");
    if (icon_filename) {
        writer.Append("1 ICON \"");
        writer.Append(icon_filename);
        writer.Append("\"\n");
    }
    writer.Append("1 24 {\n\t\"");
    doc->save(writer);
    writer.Append("\"\n}\n");

    return writer.IsValid();
}

static bool UpdateResourceFile(const char *target_name, const char *icon_filename, bool fake, const char *dest_filename)
{
    static const char *const manifest = R"(
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly manifestVersion="1.0" xmlns="urn:schemas-microsoft-com:asm.v1" xmlns:asmv3="urn:schemas-microsoft-com:asm.v3">
    <assemblyIdentity type="win32" name="" version="1.0.0.0"/>
    <application>
        <windowsSettings>
            <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
            <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
            <heapType xmlns="http://schemas.microsoft.com/SMI/2020/WindowsSettings">SegmentHeap</heapType>
        </windowsSettings>
    </application>
    <asmv3:application>
        <asmv3:windowsSettings>
            <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>
            <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
        </asmv3:windowsSettings>
    </asmv3:application>
    <dependency>
        <dependentAssembly>
            <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
                              processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
        </dependentAssembly>
    </dependency>
</assembly>
)";

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(manifest);
    RG_ASSERT(result);

    pugi::xml_node identity = doc.select_node("/assembly/assemblyIdentity").node();
    identity.attribute("name").set_value(target_name);

    LocalArray<char, 2048> res;
    if (!AssembleResourceFile(&doc, icon_filename, &res))
        return false;

    bool new_manifest;
    if (TestFile(dest_filename, FileType::File)) {
        char old_res[2048] = {};
        {
            StreamReader reader(dest_filename);
            reader.Read(RG_SIZE(old_res) - 1, old_res);
        }

        new_manifest = !TestStr(old_res, res);
    } else {
        new_manifest = true;
    }

    if (!fake && new_manifest) {
        return WriteFile(res, dest_filename);
    } else {
        return true;
    }
}

static bool CreatePrecompileHeader(const char *pch_filename, const char *dest_filename)
{
    if (!EnsureDirectoryExists(dest_filename))
        return false;

    StreamWriter writer(dest_filename);
    Print(&writer, "#include \"%1%/%2\"", GetWorkingDirectory(), pch_filename);
    return writer.Close();
}

Builder::Builder(const BuildSettings &build)
    : build(build)
{
    RG_ASSERT(build.output_directory);
    RG_ASSERT(build.compiler);

    const char *short_platform = SplitStrReverse(HostPlatformNames[(int)build.compiler->platform], '/').ptr;

    cache_directory = Fmt(&str_alloc, "%1%/%2_%3", build.output_directory, short_platform, build.compiler->name).ptr;
    shared_directory = Fmt(&str_alloc, "%1%/Shared", build.output_directory).ptr;
    cache_filename = Fmt(&str_alloc, "%1%/FelixCache.bin", shared_directory).ptr;

    if (build.compiler->platform == HostPlatform::Windows) {
        archive_filter = "*.lib";
        lib_prefix = "";
        import_extension = ".lib";
    } else {
        archive_filter = "*.a";
        lib_prefix = "lib";
        import_extension = ".so";
    }

    LoadCache();
}

static const char *GetLastDirectoryAndName(const char *filename)
{
    Span<const char> remain = filename;
    const char *name;

    SplitStrReverseAny(remain, RG_PATH_SEPARATORS, &remain);
    name = SplitStrReverseAny(remain, RG_PATH_SEPARATORS, &remain).ptr;

    return name;
}

// Beware, failures can leave the Builder in a undefined state
bool Builder::AddTarget(const TargetInfo &target, const char *version_str)
{
    HeapArray<const char *> obj_filenames;
    HeapArray<const char *> pack_filenames;
    HeapArray<const char *> link_libraries;
    HeapArray<const char *> predep_filenames;
    HeapArray<const char *> qrc_filenames;

    // Should we link this target?
    bool link = false;
    switch (target.type) {
        case TargetType::Executable: { link = true; } break;
        case TargetType::Library: {
            uint32_t features = target.CombineFeatures(build.features);
            link = (features & (int)CompileFeature::LinkLibrary);
        } break;
    }

    // Core platform source files (e.g. Teensy core)
    TargetInfo *core = nullptr;
    const char *ns = nullptr;
    {
        HeapArray<const char *> core_filenames;
        HeapArray<const char *> core_definitions;
        if (!build.compiler->GetCore(target.definitions, &str_alloc,
                                     &core_filenames, &core_definitions, &ns))
            return false;

        if (core_filenames.len) {
            RG_ASSERT(ns);

            core = core_targets_map.FindValue(ns, nullptr);

            if (!core) {
                core = core_targets.AppendDefault();

                core->name = Fmt(&str_alloc, "Cores/%1", ns).ptr;
                core->type = TargetType::Library;
                core->platforms = 1u << (int)build.compiler->platform;
                std::swap(core->definitions, core_definitions);
                core->disable_features = (int)CompileFeature::Warnings;

                for (const char *core_filename: core_filenames) {
                    SourceFileInfo src = {};

                    src.target = core;
                    src.filename = core_filename;
                    DetermineSourceType(core_filename, &src.type);

                    const SourceFileInfo *ptr = core_sources.Append(src);
                    core->sources.Append(ptr);
                }

                core_targets_map.Set(ns, core);
            }
        }
    }

    // Core object files (e.g. Teensy core)
    if (core) {
        for (const SourceFileInfo *src: core->sources) {
            RG_ASSERT(src->type == SourceType::C || src->type == SourceType::Cxx);

            if (!AddCppSource(*src, core->name, &obj_filenames))
                return false;
        }
    }

    Size prev_obj_filenames = obj_filenames.len;

    // Deal with user source files
    for (const SourceFileInfo *src: target.sources) {
        switch (src->type) {
            case SourceType::C:
            case SourceType::Cxx: {
                if (!AddCppSource(*src, ns, &obj_filenames))
                    return false;
            } break;

            case SourceType::Esbuild: {
                const char *meta_filename = AddEsbuildSource(*src);
                if (!meta_filename)
                    return false;

                meta_filename = Fmt(&str_alloc, "@%1", meta_filename).ptr;
                pack_filenames.Append(meta_filename);
            } break;

            case SourceType::QtUi: {
                const char *header_filename = AddQtUiSource(*src);
                predep_filenames.Append(header_filename);
            } break;

            case SourceType::QtResources: {
                qrc_filenames.Append(src->filename);
            } break;
        }
    }

    // Build Qt resource file
    if (qrc_filenames.len) {
        const char *obj_filename = AddQtResource(target, qrc_filenames);
        obj_filenames.Append(obj_filename);
    }

    // Make sure C/C++ source files must depend on generated headers
    for (Size i = prev_obj_filenames; i < obj_filenames.len; i++) {
        const char *obj_filename = obj_filenames[i];
        Size node_idx = nodes_map.FindValue(obj_filename, -1);

        if (node_idx >= 0) {
            Node *node = &nodes[node_idx];

            for (const char *predep_filename: predep_filenames) {
                Size src_idx = nodes_map.FindValue(predep_filename, -1);

                if (src_idx >= 0) {
                    Node *src = &nodes[src_idx];

                    src->triggers.Append(node_idx);
                    node->semaphore++;
                }
            }
        }
    }

    // User assets and libraries
    pack_filenames.Append(target.pack_filenames);
    link_libraries.Append(target.libraries);

    // Assets
    if (pack_filenames.len) {
        const char *src_filename = Fmt(&str_alloc, "%1%/Misc%/%2_assets.c", cache_directory, target.name).ptr;
        const char *obj_filename = Fmt(&str_alloc, "%1%2", src_filename, build.compiler->GetObjectExtension()).ptr;

        uint32_t features = target.CombineFeatures(build.features);
        bool module = (features & (int)CompileFeature::HotAssets);

        // Make C file
        {
            Command cmd = {};
            build.compiler->MakePackCommand(pack_filenames, target.pack_options, src_filename, &str_alloc, &cmd);

            const char *text = Fmt(&str_alloc, "Pack %!..+%1%!0 assets", target.name).ptr;
            AppendNode(text, src_filename, cmd, pack_filenames, nullptr);
        }

        // Build object file
        {
            Command cmd = {};
            if (module) {
                build.compiler->MakeObjectCommand(src_filename, SourceType::C,
                                                  nullptr, {"EXPORT"}, {}, {}, {}, features, build.env,
                                                  obj_filename, &str_alloc, &cmd);
            } else {
                build.compiler->MakeObjectCommand(src_filename, SourceType::C,
                                                  nullptr, {}, {}, {}, {}, features, build.env,
                                                  obj_filename,  &str_alloc, &cmd);
            }

            const char *text = Fmt(&str_alloc, "Compile %!..+%1%!0 assets", target.name).ptr;
            AppendNode(text, obj_filename, cmd, src_filename, nullptr);
        }

        // Build module if needed
        if (module) {
            const char *module_filename = Fmt(&str_alloc, "%1%/%2_assets%3", build.output_directory,
                                              target.name, RG_SHARED_LIBRARY_EXTENSION).ptr;

            Command cmd = {};
            build.compiler->MakeLinkCommand(obj_filename, {}, TargetType::Library,
                                            features, build.env, module_filename, &str_alloc, &cmd);

            const char *text = Fmt(&str_alloc, "Link %!..+%1%!0", GetLastDirectoryAndName(module_filename)).ptr;
            AppendNode(text, module_filename, cmd, obj_filename, nullptr);
        } else {
            obj_filenames.Append(obj_filename);
        }
    }

    // Some compilers (such as MSVC) also build PCH object files that need to be linked
    if (build.features & (int)CompileFeature::PCH) {
        for (const char *filename: target.pchs) {
            const char *pch_filename = build_map.FindValue({ ns, filename }, nullptr);

            if (pch_filename) {
                const char *obj_filename = build.compiler->GetPchObject(pch_filename, &str_alloc);

                if (obj_filename) {
                    obj_filenames.Append(obj_filename);
                }
            }
        }
    }

    // Version string
    if (target.type == TargetType::Executable) {
        const char *src_filename = Fmt(&str_alloc, "%1%/Misc%/%2.c", cache_directory, target.name).ptr;
        const char *obj_filename = Fmt(&str_alloc, "%1%2", src_filename, build.compiler->GetObjectExtension()).ptr;
        uint32_t features = target.CombineFeatures(build.features);

        if (!UpdateVersionSource(target.name, version_str, src_filename))
            return false;

        Command cmd = {};
        build.compiler->MakeObjectCommand(src_filename, SourceType::C,
                                          nullptr, {}, {}, {}, {}, features, build.env,
                                          obj_filename, &str_alloc, &cmd);

        const char *text = Fmt(&str_alloc, "Compile %!..+%1%!0 version file", target.name).ptr;
        AppendNode(text, obj_filename, cmd, src_filename, ns);

        obj_filenames.Append(obj_filename);
    }

    // Resource file (Windows only)
    if (build.compiler->platform == HostPlatform::Windows &&
            target.type == TargetType::Executable) {
        const char *rc_filename = Fmt(&str_alloc, "%1%/Misc%/%2.rc", cache_directory, target.name).ptr;
        const char *res_filename = Fmt(&str_alloc, "%1%/Misc%/%2.res", cache_directory, target.name).ptr;

        if (!UpdateResourceFile(target.name, target.icon_filename, build.fake, rc_filename))
            return false;

        Command cmd = {};
        build.compiler->MakeResourceCommand(rc_filename, res_filename, &str_alloc, &cmd);

        const char *text = Fmt(&str_alloc, "Build %!..+%1%!0 resource file", target.name).ptr;
        if (target.icon_filename) {
            AppendNode(text, res_filename, cmd, { rc_filename, target.icon_filename }, ns);
        } else {
            AppendNode(text, res_filename, cmd, rc_filename, ns);
        }

        obj_filenames.Append(res_filename);
    }

    // Link with required Qt libraries
    if (target.qt_components.len && !AddQtLibraries(target, &obj_filenames, &link_libraries))
        return false;

    // Link commands
    if (link) {
        const char *link_ext = build.compiler->GetLinkExtension(target.type);
        const char *post_ext = build.compiler->GetPostExtension(target.type);

        // Generate linked output
        const char *link_filename;
        {
            link_filename = Fmt(&str_alloc, "%1%/%2%3", build.output_directory, target.title, link_ext).ptr;
            uint32_t features = target.CombineFeatures(build.features);

            Command cmd = {};
            build.compiler->MakeLinkCommand(obj_filenames, link_libraries, target.type,
                                            features, build.env, link_filename, &str_alloc, &cmd);

            const char *text = Fmt(&str_alloc, "Link %!..+%1%!0", GetLastDirectoryAndName(link_filename)).ptr;
            AppendNode(text, link_filename, cmd, obj_filenames, ns);
        }

        const char *target_filename;
        if (post_ext) {
            target_filename = Fmt(&str_alloc, "%1%/%2%3", build.output_directory, target.title, post_ext).ptr;

            Command cmd = {};
            build.compiler->MakePostCommand(link_filename, target_filename, &str_alloc, &cmd);

            const char *text = Fmt(&str_alloc, "Convert %!..+%1%!0", GetLastDirectoryAndName(target_filename)).ptr;
            AppendNode(text, target_filename, cmd, link_filename, ns);
        } else {
            target_filename = link_filename;
        }

#ifdef __APPLE__
        // Bundle macOS GUI apps
        if (build.compiler->platform == HostPlatform::macOS && target.qt_components.len) {
            const char *bundle_filename = Fmt(&str_alloc, "%1.app", target_filename).ptr;

            Command cmd = {};

            {
                HeapArray<char> buf(&str_alloc);

                Fmt(&buf, "\"%1\" macify -f \"%2\" -O \"%3\"", GetApplicationExecutable(), target_filename, bundle_filename);
                if (target.icon_filename) {
                    Fmt(&buf, " --icon \"%1\"", target.icon_filename);
                }
                Fmt(&buf, " --title \"%1\" --qmake_path \"%2\"", target.title, qt->qmake);

                cmd.cache_len = buf.len;
                cmd.cmd_line = buf.TrimAndLeak(1);
            }

            const char *text = Fmt(&str_alloc, "Bundle %!..+%1%!0", GetLastDirectoryAndName(bundle_filename)).ptr;
            AppendNode(text, bundle_filename, cmd, target_filename, ns);

            target_filename = bundle_filename;
        }
#endif

        target_filenames.Set(target.name, target_filename);
    }

    return true;
}

bool Builder::AddSource(const SourceFileInfo &src)
{
    switch (src.type) {
        case SourceType::C:
        case SourceType::Cxx: return AddCppSource(src, nullptr, nullptr);
        case SourceType::Esbuild: return AddEsbuildSource(src);
        case SourceType::QtUi: return AddQtUiSource(src);

        case SourceType::QtResources: {
            LogInfo("You cannot build QRC files directly");
            return false;
        }
    }

    RG_UNREACHABLE();
}

bool Builder::AddCppSource(const SourceFileInfo &src, const char *ns, HeapArray<const char *> *out_objects)
{
    RG_ASSERT(src.type == SourceType::C || src.type == SourceType::Cxx);

    // Precompiled header (if any)
    const char *pch_filename = nullptr;
    if (build.features & (int)CompileFeature::PCH) {
        const SourceFileInfo *pch = nullptr;
        const char *pch_ext = nullptr;
        switch (src.type) {
            case SourceType::C: {
                pch = src.target->c_pch_src;
                pch_ext = ".c";
            } break;
            case SourceType::Cxx: {
                pch = src.target->cxx_pch_src;
                pch_ext = ".cc";
            } break;

            case SourceType::Esbuild:
            case SourceType::QtUi:
            case SourceType::QtResources: { RG_UNREACHABLE(); } break;
        }

        if (pch) {
            pch_filename = build_map.FindValue({ ns, pch->filename }, nullptr);

            if (!pch_filename) {
                pch_filename = BuildObjectPath(ns, pch->filename, cache_directory, pch_ext);

                const char *cache_filename = build.compiler->GetPchCache(pch_filename, &str_alloc);

                uint32_t features = build.features;
                features = pch->target->CombineFeatures(features);
                features = pch->CombineFeatures(features);

                Command cmd = {};
                build.compiler->MakePchCommand(pch_filename, pch->type,
                                               pch->target->definitions, pch->target->include_directories,
                                               pch->target->include_files, features, build.env, &str_alloc, &cmd);

                // Check the PCH cache file against main file dependencies
                if (!IsFileUpToDate(cache_filename, pch_filename)) {
                    mtime_map.Set(pch_filename, -1);
                } else {
                    const CacheEntry *entry = cache_map.Find(pch_filename);

                    if (!entry) {
                        mtime_map.Set(pch_filename, -1);
                    } else {
                        Span<const DependencyEntry> dependencies = MakeSpan(cache_dependencies.ptr + entry->deps_offset, entry->deps_len);

                        if (!IsFileUpToDate(cache_filename, dependencies)) {
                            mtime_map.Set(pch_filename, -1);
                        }
                    }
                }

                const char *text = Fmt(&str_alloc, "Precompile %!..+%1%!0", pch->filename).ptr;
                if (AppendNode(text, pch_filename, cmd, pch->filename, ns)) {
                    if (!build.fake && !CreatePrecompileHeader(pch->filename, pch_filename))
                        return false;
                }
            }
        }
    }

    const char *obj_filename = build_map.FindValue({ ns, src.filename }, nullptr);

    // Build main object
    if (!obj_filename) {
        obj_filename = BuildObjectPath(ns, src.filename, cache_directory, build.compiler->GetObjectExtension());

        uint32_t features = build.features;
        features = src.target->CombineFeatures(features);
        features = src.CombineFeatures(features);

        HeapArray<const char *> system_directories;
        if (src.target->qt_components.len && !AddQtDirectories(src, &system_directories))
            return false;

        Command cmd = {};
        build.compiler->MakeObjectCommand(src.filename, src.type,
                                          pch_filename, src.target->definitions,
                                          src.target->include_directories, system_directories,
                                          src.target->include_files, features, build.env,
                                          obj_filename, &str_alloc, &cmd);

        const char *text = Fmt(&str_alloc, "Compile %!..+%1%!0", src.filename).ptr;
        if (pch_filename ? AppendNode(text, obj_filename, cmd, { src.filename, pch_filename }, ns)
                         : AppendNode(text, obj_filename, cmd, src.filename, ns)) {
            if (!build.fake && !EnsureDirectoryExists(obj_filename))
                return false;
        }

        if (src.target->qt_components.len && !CompileMocHelper(src, system_directories, features))
            return false;
    }

    if (out_objects) {
        out_objects->Append(obj_filename);

        if (src.target->qt_components.len) {
            const char *moc_obj = moc_map.FindValue(src.filename, nullptr);

            if (moc_obj) {
                out_objects->Append(moc_obj);
            }
        }
    }

    return true;
}

bool Builder::UpdateVersionSource(const char *target, const char *version, const char *dest_filename)
{
    if (!build.fake && !EnsureDirectoryExists(dest_filename))
        return false;

    char code[1024];
    Fmt(code, "// This file is auto-generated by felix\n\n"
              "const char *FelixTarget = \"%1\";\n"
              "const char *FelixVersion = \"%2\";\n"
              "const char *FelixCompiler = \"%3 (%4)\";\n",
        target, version ? version : "(unknown version)",
        build.compiler->name, FmtFlags(build.features, CompileFeatureOptions));

    bool new_version;
    if (TestFile(dest_filename, FileType::File)) {
        char old_code[1024] = {};
        ReadFile(dest_filename, MakeSpan(old_code, RG_SIZE(old_code) - 1));

        new_version = !TestStr(old_code, code);
    } else {
        new_version = true;
    }

    if (!build.fake && new_version) {
        return WriteFile(code, dest_filename);
    } else {
        return true;
    }
}

const char *Builder::GetTargetIncludeDirectory(const TargetInfo &target)
{
    bool inserted;
    const char **ptr = target_headers.TrySet(target.name, "", &inserted);

    if (inserted) {
        *ptr = Fmt(&str_alloc, "%1%/Headers/%2", cache_directory, target.name).ptr;
    }

    return *ptr;
}

bool Builder::Build(int jobs, bool verbose)
{
    RG_ASSERT(jobs >= 0);

    // Reset build context
    clear_filenames.Clear();
    rsp_map.Clear();
    progress = total - nodes.len;
    workers.Clear();
    workers.AppendDefault(jobs);

    RG_DEFER {
        // Update cache even if some tasks fail
        if (nodes.len && !build.fake) {
            for (const WorkerState &worker: workers) {
                for (const CacheEntry &entry: worker.entries) {
                    cache_map.Set(entry)->deps_offset = cache_dependencies.len;
                    for (Size i = 0; i < entry.deps_len; i++) {
                        DependencyEntry dep = {};

                        dep.filename = DuplicateString(worker.dependencies[entry.deps_offset + i], &str_alloc).ptr;
                        dep.mtime = GetFileModificationTime(dep.filename, true);

                        cache_dependencies.Append(dep);
                    }
                }
            }
            workers.Clear();

            SaveCache();
        }

        // Clean up failed and temporary files
        // Windows has a tendency to hold file locks a bit longer than needed...
        // Try to delete files several times silently unless it's the last try.
#ifdef _WIN32
        if (clear_filenames.len) {
            PushLogFilter([](LogLevel, const char *, const char *, FunctionRef<LogFunc>) {});
            RG_DEFER { PopLogFilter(); };

            for (Size i = 0; i < 3; i++) {
                bool success = true;
                for (const char *filename: clear_filenames) {
                    success &= UnlinkFile(filename);
                }

                if (success)
                    return;

                WaitDelay(150);
            }
        }
#endif
        for (const char *filename: clear_filenames) {
            UnlinkFile(filename);
        }
    };

    // Replace long command lines with response files if the command supports it
    if (!build.fake) {
        for (const Node &node: nodes) {
            const Command &cmd = node.cmd;

            if (cmd.cmd_line.len > MAX_COMMAND_LEN && cmd.rsp_offset > 0) {
                RG_ASSERT(cmd.rsp_offset < cmd.cmd_line.len);

                // In theory, there can be conflicts between RSP files. But it is unlikely
                // that response files will be generated for anything other than link commands,
                // so the risk is very low.
                const char *target_basename = SplitStrReverseAny(node.dest_filename, RG_PATH_SEPARATORS).ptr;
                const char *rsp_filename = Fmt(&str_alloc, "%1%/Misc%/%2.rsp", cache_directory, target_basename).ptr;

                if (!EnsureDirectoryExists(rsp_filename))
                    return false;

                Span<const char> rsp = cmd.cmd_line.Take(cmd.rsp_offset + 1,
                                                         cmd.cmd_line.len - cmd.rsp_offset - 1);

                // Apparently backslash characters needs to be escaped in response files,
                // but it's easier to use '/' instead.
                StreamWriter st(rsp_filename);
                for (char c: rsp) {
                    st.Write(c == '\\' ? '/' : c);
                }
                if (!st.Close())
                    return false;

                const char *new_cmd = Fmt(&str_alloc, "%1 \"@%2\"",
                                          cmd.cmd_line.Take(0, cmd.rsp_offset), rsp_filename).ptr;
                rsp_map.Set(&node, new_cmd);
            }
        }
    }

    LogInfo("Building with %1 %2...", jobs, jobs > 1 ? "threads" : "thread");
    int64_t now = GetMonotonicTime();

    Async async(jobs, build.stop_after_error);

    // Run nodes
    bool busy = false;
    for (Size i = 0; i < nodes.len; i++) {
        Node *node = &nodes[i];

        if (!node->success && !node->semaphore) {
            async.Run([=, &async, this]() { return RunNode(&async, node, verbose); });
            busy = true;
        }
    }

    if (async.Sync()) {
        if (busy) {
            if (!build.fake) {
                double time = (double)(GetMonotonicTime() - now) / 1000.0;
                LogInfo("Done (%1s)", FmtDouble(time, 1));
            } else {
                LogInfo("Done %!D..[dry run]%!0");
            }
        } else {
            LogInfo("Nothing to do!%!D..%1%!0", build.fake ? " [dry run]" : "");
        }

        return true;
    } else if (WaitForInterrupt(0) == WaitForResult::Interrupt) {
        LogError("Build was interrupted");
        return false;
    } else {
        if (!build.stop_after_error) {
            LogError("Some errors have occured (use %!..+felix -s%!0 to stop after first error)");
        }
        return false;
    }
}

void Builder::SaveCache()
{
    if (!EnsureDirectoryExists(cache_filename))
        return;

    StreamWriter st(cache_filename, (int)StreamWriterFlag::Atomic);
    if (!st.IsValid())
        return;

    for (const CacheEntry &entry: cache_map) {
        PrintLn(&st, "%1>%2", entry.deps_len, entry.filename);
        PrintLn(&st, "%1", entry.cmd_line);
        for (Size i = 0; i < entry.deps_len; i++) {
            const DependencyEntry &dep = cache_dependencies[entry.deps_offset + i];
            PrintLn(&st, "%1|%2", dep.mtime, dep.filename);
        }
    }

    if (!st.Close())
        return;
}

void Builder::LoadCache()
{
    if (!TestFile(cache_filename))
        return;

    RG_DEFER_N(clear_guard) {
        cache_map.Clear();
        cache_dependencies.Clear();

        LogError("Purging cache file '%1'", cache_filename);
        UnlinkFile(cache_filename);
    };

    // Load whole file to memory
    HeapArray<char> cache(&str_alloc);
    if (ReadFile(cache_filename, Megabytes(64), &cache) < 0)
        return;
    cache.len = TrimStrRight(cache.Take(), "\n").len;
    cache.Grow(1);

    // Parse cache file
    {
        Span<char> remain = cache;

        while (remain.len) {
            CacheEntry entry = {};

            // Filename and dependency count
            {
                Span<char> part = SplitStr(remain, '>', &remain);
                if (!ParseInt(part, &entry.deps_len))
                    return;
                entry.deps_offset = cache_dependencies.len;

                part = SplitStr(remain, '\n', &remain);
                part.ptr[part.len] = 0;
                entry.filename = part.ptr;
            }

            // Command line
            {
                Span<char> part = SplitStr(remain, '\n', &remain);
                part.ptr[part.len] = 0;
                entry.cmd_line = part;
            }

            // Dependencies
            for (Size i = 0; i < entry.deps_len; i++) {
                DependencyEntry dep = {};

                Span<char> part = SplitStr(remain, '|', &remain);
                if (!ParseInt(part, &dep.mtime))
                    return;

                part = SplitStr(remain, '\n', &remain);
                part.ptr[part.len] = 0;
                dep.filename = part.ptr;

                cache_dependencies.Append(dep);
            }
            entry.deps_len = cache_dependencies.len - entry.deps_offset;

            cache_map.Set(entry);
        }
    }

    cache.Leak();
    clear_guard.Disable();
}

const char *Builder::BuildObjectPath(const char *ns, const char *src_filename,
                                     const char *output_directory, const char *suffix)
{
    if (PathIsAbsolute(src_filename)) {
        src_filename += strcspn(src_filename, RG_PATH_SEPARATORS) + 1;
    }

    HeapArray<char> buf(&str_alloc);

    Size offset;
    if (ns) {
        offset = Fmt(&buf, "%1%/Objects%/@%2%/", output_directory, ns).len;
    } else {
        offset = Fmt(&buf, "%1%/Objects%/", output_directory).len;
    }
    Fmt(&buf, "%1%2", src_filename, suffix);

    // Replace '..' components with '__'
    {
        char *ptr = buf.ptr + offset;

        while ((ptr = strstr(ptr, ".."))) {
            if (IsPathSeparator(ptr[-1]) && (IsPathSeparator(ptr[2]) || !ptr[2])) {
                ptr[0] = '_';
                ptr[1] = '_';
            }

            ptr += 2;
        }
    }

    return buf.TrimAndLeak(1).ptr;
}

static inline const char *CleanFileName(const char *str)
{
    return str + (str[0] == '@');
}

bool Builder::AppendNode(const char *text, const char *dest_filename, const Command &cmd,
                         Span<const char *const> src_filenames, const char *ns)
{
    RG_ASSERT(src_filenames.len >= 1);

    build_map.Set({ ns, CleanFileName(src_filenames[0]) }, dest_filename);
    total++;

    if (NeedsRebuild(dest_filename, cmd, src_filenames)) {
        Size node_idx = nodes.len;
        Node *node = nodes.AppendDefault();

        node->text = text;
        node->dest_filename = dest_filename;
        node->cmd = cmd;

        // Add triggers to source file nodes
        for (const char *src_filename: src_filenames) {
            src_filename = CleanFileName(src_filename);

            Size src_idx = nodes_map.FindValue(src_filename, -1);

            if (src_idx >= 0) {
                Node *src = &nodes[src_idx];

                src->triggers.Append(node_idx);
                node->semaphore++;
            }
        }

        nodes_map.Set(dest_filename, node_idx);
        mtime_map.Set(dest_filename, -1);

        return true;
    } else {
        return false;
    }
}

bool Builder::NeedsRebuild(const char *dest_filename, const Command &cmd,
                           Span<const char *const> src_filenames)
{
    const CacheEntry *entry = cache_map.Find(dest_filename);

    if (!entry)
        return true;
    if (!TestStr(cmd.cmd_line.Take(0, cmd.cache_len), entry->cmd_line))
        return true;

    if (!IsFileUpToDate(dest_filename, src_filenames))
        return true;

    Span<const DependencyEntry> dependencies = MakeSpan(cache_dependencies.ptr + entry->deps_offset, entry->deps_len);

    if (!IsFileUpToDate(dest_filename, dependencies))
        return true;

    return false;
}

bool Builder::IsFileUpToDate(const char *dest_filename, Span<const char *const> src_filenames)
{
    if (build.rebuild)
        return false;

    int64_t dest_time = GetFileModificationTime(dest_filename);
    if (dest_time < 0)
        return false;

    for (const char *src_filename: src_filenames) {
        src_filename = CleanFileName(src_filename);

        int64_t src_time = GetFileModificationTime(src_filename);
        if (src_time < 0 || src_time > dest_time)
            return false;
    }

    return true;
}

bool Builder::IsFileUpToDate(const char *dest_filename, Span<const DependencyEntry> dependencies)
{
    if (build.rebuild)
        return false;

    int64_t dest_time = GetFileModificationTime(dest_filename);
    if (dest_time < 0)
        return false;

    for (const DependencyEntry &dep: dependencies) {
        int64_t dep_time = GetFileModificationTime(dep.filename);

        if (dep_time < 0 || dep_time > dest_time)
            return false;
        if (dep_time != dep.mtime)
            return false;
    }

    return true;
}

int64_t Builder::GetFileModificationTime(const char *filename, bool retry)
{
    int64_t *ptr = mtime_map.Find(filename);

    if (ptr && (*ptr >= 0 || !retry)) {
        return *ptr;
    } else {
        FileInfo file_info;
        if (StatFile(filename, (int)StatFlag::IgnoreMissing, &file_info) != StatResult::Success)
            return -1;

        // filename might be temporary (e.g. dependency filenames in NeedsRebuild())
        const char *filename2 = DuplicateString(filename, &str_alloc).ptr;
        mtime_map.Set(filename2, file_info.mtime);

        return file_info.mtime;
    }
}

static Span<const char> ParseMakeFragment(Span<const char> remain, HeapArray<char> *out_frag)
{
    // Skip white spaces
    remain = TrimStrLeft(remain);

    if (remain.len) {
        out_frag->Append(remain[0]);

        Size i = 1;
        for (; i < remain.len && !strchr("\r\n", remain[i]); i++) {
            char c = remain[i];

            if (strchr(" $#:", c)) {
                if (remain[i - 1] == '\\') {
                    (*out_frag)[out_frag->len - 1] = c;
#ifdef _WIN32
                } else if (c == ':' && i == 1) {
                    // Absolute Windows paths start with [A-Z]:
                    // Some MinGW builds escape the colon, some don't. Tolerate both cases.
                    out_frag->Append(c);
#endif
                } else {
                    break;
                }
            } else {
                out_frag->Append(c);
            }
        }

        remain = remain.Take(i, remain.len - i);
    }

    return remain;
}

static bool ParseMakeRule(const char *filename, Allocator *alloc, HeapArray<const char *> *out_filenames)
{
    RG_ASSERT(alloc);

    HeapArray<char> rule_buf;
    if (ReadFile(filename, Megabytes(2), &rule_buf) < 0)
        return false;

    // Parser state
    Span<const char> remain = rule_buf;
    HeapArray<char> frag;

    // Skip outputs
    while (remain.len) {
        frag.RemoveFrom(0);
        remain = ParseMakeFragment(remain, &frag);

        if (TestStr(frag, ":"))
            break;
    }

    // Get dependency filenames
    while (remain.len) {
        frag.RemoveFrom(0);
        remain = ParseMakeFragment(remain, &frag);

        if (frag.len && !TestStr(frag, "\\")) {
            const char *dep_filename = NormalizePath(frag, alloc).ptr;
            out_filenames->Append(dep_filename);
        }
    }

    return true;
}

static Size ExtractShowIncludes(Span<char> buf, Allocator *alloc, HeapArray<const char *> *out_filenames)
{
    RG_ASSERT(alloc || !out_filenames);

    // We need to strip include notes from the output
    Span<char> new_buf = MakeSpan(buf.ptr, 0);

    while (buf.len) {
        Span<const char> line = SplitStr(buf, '\n', &buf);

        // MS had the brilliant idea to localize inclusion notes.. In english it starts
        // with 'Note: including file: ' but it can basically be anything. We match
        // lines that start with a non-space character, with two pairs of ': ' not
        // preceeded by any digit. Meh.
        Span<const char> dep = {};
        if (line.len && !IsAsciiWhite(line[0])) {
            int counter = 0;

            for (Size i = 0; i < line.len - 2; i++) {
                if (IsAsciiDigit(line[i]))
                    break;

                counter += (line[i] == ':' && line[i + 1] == ' ');

                if (counter == 2) {
                    dep = TrimStr(line.Take(i + 2, line.len - i - 2));
                    break;
                }
            }
        }

        if (dep.len) {
            if (out_filenames) {
                const char *dep_filename = DuplicateString(dep, alloc).ptr;
                out_filenames->Append(dep_filename);
            }
        } else {
            Size copy_len = line.len + (buf.ptr > line.end());

            memmove_safe(new_buf.end(), line.ptr, copy_len);
            new_buf.len += copy_len;
        }
    }

    return new_buf.len;
}

static bool ParseEsbuildMeta(const char *filename, Allocator *alloc, HeapArray<const char *> *out_filenames)
{
    RG_ASSERT(alloc);

    RG_DEFER_NC(err_guard, len = out_filenames->len) {
        out_filenames->RemoveFrom(len);
        UnlinkFile(filename);
    };

    StreamReader reader(filename);
    if (!reader.IsValid())
        return false;
    json_Parser parser(&reader, alloc);

    StreamWriter writer(filename, (int)StreamWriterFlag::Atomic);
    if (!writer.IsValid())
        return false;

    parser.ParseObject();
    while (parser.InObject()) {
        Span<const char> key = {};
        parser.ParseKey(&key);

        if (key == "inputs") {
            parser.ParseObject();
            while (parser.InObject()) {
                Span<const char> filename = {};
                parser.ParseKey(&filename);

                const char *dep_filename = NormalizePath(filename, alloc).ptr;
                out_filenames->Append(dep_filename);

                parser.Skip();
            }
        } else if (key == "outputs") {
            Size prefix_len = -1;

            parser.ParseObject();
            while (parser.InObject()) {
                Span<const char> filename = {};
                parser.ParseKey(&filename);

                // The first entry contrains entryPoint which we need to fix all paths
                if (prefix_len < 0) {
                    parser.ParseObject();
                    while (parser.InObject()) {
                        Span<const char> key = {};
                        parser.ParseKey(&key);

                        if (key == "entryPoint") {
                            Span<const char> entry_point = {};
                            parser.ParseString(&entry_point);

                            prefix_len = filename.len - entry_point.len;
                        } else {
                            parser.Skip();
                        }
                    }

                    if (prefix_len < 0) {
                        LogError("Failed to read output files from esbuild meta file");
                        return false;
                    }
                } else {
                    parser.Skip();
                }

                if (filename.len > prefix_len) {
                    PrintLn(&writer, "[%1]", filename.Take(prefix_len, filename.len - prefix_len));
                    PrintLn(&writer, "File = %1", filename);
                }
            }
        } else {
            parser.Skip();
        }
    }
    if (!parser.IsValid())
        return false;
    reader.Close();

    if (!writer.Close())
        return true;

    err_guard.Disable();
    return true;
}

bool Builder::RunNode(Async *async, Node *node, bool verbose)
{
    if (WaitForInterrupt(0) == WaitForResult::Interrupt)
        return false;

    const Command &cmd = node->cmd;

    WorkerState *worker = &workers[Async::GetWorkerIdx()];
    const char *cmd_line = rsp_map.FindValue(node, cmd.cmd_line.ptr);

    // The lock is needed to guarantee ordering of progress counter. Atomics
    // do not help much because the LogInfo() calls need to be protected too.
    {
        std::lock_guard<std::mutex> out_lock(out_mutex);

        int pad = (int)log10(total) + 3;
        progress++;

        LogInfo("%!C..%1/%2%!0 %3", FmtArg(progress).Pad(-pad), total, node->text);
        if (verbose) {
            PrintLn(cmd_line);
            fflush(stdout);
        }
    }

    // Run command
    HeapArray<char> output_buf;
    int exit_code;
    bool started;
    if (!build.fake) {
        ExecuteInfo info = {};

        info.work_dir = nullptr;
        info.env_variables = cmd.env_variables;

        started = ExecuteCommandLine(cmd_line, info, {}, Megabytes(4), &output_buf, &exit_code);
    } else {
        started = true;
        exit_code = 0;
    }

    // Skip first output lines (if needed)
    Span<char> output = output_buf;
    for (Size i = 0; i < cmd.skip_lines; i++) {
        SplitStr(output, '\n', &output);
    }

    // Deal with results
    if (started && !exit_code) {
        // Update cache entries
        {
            CacheEntry entry;

            entry.filename = node->dest_filename;
            entry.cmd_line = cmd.cmd_line.Take(0, cmd.cache_len);

            entry.deps_offset = worker->dependencies.len;
            switch (cmd.deps_mode) {
                case Command::DependencyMode::None: {} break;
                case Command::DependencyMode::MakeLike: {
                    if (TestFile(cmd.deps_filename)) {
                        started &= ParseMakeRule(cmd.deps_filename, &worker->str_alloc, &worker->dependencies);
                        UnlinkFile(cmd.deps_filename);
                    }
                } break;
                case Command::DependencyMode::ShowIncludes: {
                    output.len = ExtractShowIncludes(output, &worker->str_alloc, &worker->dependencies);
                } break;
                case Command::DependencyMode::EsbuildMeta: {
                    if (TestFile(cmd.deps_filename)) {
                        started &= ParseEsbuildMeta(cmd.deps_filename, &worker->str_alloc, &worker->dependencies);
                    }
                } break;
            }
            entry.deps_len = worker->dependencies.len - entry.deps_offset;

            worker->entries.Append(entry);
        }

        if (output.len) {
            std::lock_guard<std::mutex> out_lock(out_mutex);
            stdout_st.Write(output);
        }

        // Trigger dependent nodes
        for (Size trigger_idx: node->triggers) {
            Node *trigger = &nodes[trigger_idx];

            if (!--trigger->semaphore) {
                RG_ASSERT(!trigger->success);
                async->Run([=, this]() { return RunNode(async, trigger, verbose); });
            }
        }

        node->success = true;
        return true;
    } else {
        std::lock_guard<std::mutex> out_lock(out_mutex);

        // Even through we don't care about dependencies, we still want to
        // remove include notes from the output buffer.
        if (cmd.deps_mode == Command::DependencyMode::ShowIncludes) {
            output.len = ExtractShowIncludes(output, nullptr, nullptr);
        }

        cache_map.Remove(node->dest_filename);
        clear_filenames.Append(node->dest_filename);

        if (!started) {
            // Error already issued by ExecuteCommandLine()
            stderr_st.Write(output);
        } else if (WaitForInterrupt(0) != WaitForResult::Interrupt) {
            LogError("%1 %!..+(exit code %2)%!0", node->text, exit_code);
            stderr_st.Write(output);
        }

        return false;
    }
}

}
