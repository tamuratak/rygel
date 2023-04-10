// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "src/core/libcc/libcc.hh"
extern "C" {
    #include "vendor/libsoldout/soldout.h"
}

namespace RG {

struct PageSection {
    const char *id;
    const char *title;
    int level = 0;
};

struct PageData {
    const char *src_filename;

    const char *title;
    const char *menu;
    const char *created;
    const char *modified;
    HeapArray<PageSection> sections;

    std::shared_ptr<const char> html_buf;
    Span<const char> html;

    const char *name;
    const char *url;
};

static const char *FileNameToPageName(const char *filename, Allocator *alloc)
{
    // File name and extension
    Span<const char> name = SplitStrReverseAny(filename, RG_PATH_SEPARATORS);
    SplitStrReverse(name, '.', &name);

    // Remove leading number and underscore if any
    {
        const char *after_number;
        strtol(name.ptr, const_cast<char **>(&after_number), 10);
        if (after_number && after_number[0] == '_') {
            name = MakeSpan(after_number + 1, name.end());
        }
    }

    // Filter out unwanted characters
    char *name2 = DuplicateString(name, alloc).ptr;
    for (Size i = 0; name2[i]; i++) {
        name2[i] = IsAsciiAlphaOrDigit(name2[i]) ? name2[i] : '_';
    }

    return name2;
}

static Span<const char> TextToID(Span<const char> text, Allocator *alloc)
{
    Span<char> id = AllocateSpan<char>(alloc, text.len + 1);

    Size j = 0;
    for (Size i = 0; i < text.len; i++) {
        if (IsAsciiAlphaOrDigit(text[i])) {
            id[j++] = LowerAscii(text[i]);
        } else {
            id[j++] = '_';

            while (++i < text.len && !IsAsciiAlphaOrDigit(text[i]));
            i--;
        }
    }

    id.len = j;
    id.ptr[j] = 0;

    return id;
}

// XXX: Resolve page links in content
static bool RenderPageContent(PageData *page, Allocator *alloc)
{
    buf *ib = bufnew(1024);
    RG_DEFER { bufrelease(ib); };

    // Load the file, struct buf is used by libsoldout
    {
        StreamReader st(page->src_filename);

        Size bytes_read;
        bufgrow(ib, 1024);
        while ((bytes_read = st.Read(ib->asize - ib->size, ib->data + ib->size)) > 0) {
            ib->size += bytes_read;
            bufgrow(ib, ib->size + 1024);
        }

        if (!st.IsValid())
            return false;
    }

    struct RenderContext {
        Allocator *alloc;
        PageData *page;
    };

    mkd_renderer renderer = discount_html;
    RenderContext ctx = {};
    ctx.page = page;
    ctx.alloc = alloc;
    renderer.opaque = &ctx;

    // Get page sections from the parser
    renderer.header = [](buf *ob, buf *text, int level, void *udata) {
        RenderContext *ctx = (RenderContext *)udata;

        if (level < 3) {
            PageSection sec = {};

            sec.level = level;
            sec.title = DuplicateString(MakeSpan(text->data, text->size), ctx->alloc).ptr;
            sec.id = TextToID(sec.title, ctx->alloc).ptr;

            // XXX: Detect duplicate sections
            ctx->page->sections.Append(sec);

            bufprintf(ob, "<h%d id=\"%s\">%s</h%d>", level, sec.id, sec.title, level);
        } else {
            bufprintf(ob, "<h%d>%.*s</h%d>", level, (int)text->size, text->data, level);
        }
    };

    // We use HTML comments for metadata (creation date, etc.),
    // for example '<!-- Title: foobar -->' or '<!-- Created: 2016-01-12 -->'.
    renderer.blockhtml = [](buf *ob, buf *text, void *udata) {
        RenderContext *ctx = (RenderContext *)udata;

        Size size = text->size;
        while (size && text->data[size - 1] == '\n') {
            size--;
        }
        if (size >= 7 && !memcmp(text->data, "<!--", 4) &&
                         !memcmp(text->data + size - 3, "-->", 3)) {
            Span<const char> comment = MakeSpan(text->data + 4, size - 7);

            while (comment.len) {
                Span<const char> line = SplitStr(comment, '\n', &comment);

                Span<const char> value;
                Span<const char> name = TrimStr(SplitStr(line, ':', &value));
                value = TrimStr(value);

                if (value.ptr == name.end())
                    break;

                const char **attr_ptr;
                if (name == "Title") {
                    attr_ptr = &ctx->page->title;
                } else if (name == "Menu") {
                    attr_ptr = &ctx->page->menu;
                } else if (name == "Created") {
                    attr_ptr = &ctx->page->created;
                } else if (name == "Modified") {
                    attr_ptr = &ctx->page->modified;
                } else {
                    LogError("%1: Unknown attribute '%2'", ctx->page->src_filename, name);
                    continue;
                }

                if (*attr_ptr) {
                    LogError("%1: Overwriting attribute '%2' (already set)",
                            ctx->page->src_filename, name);
                }
                *attr_ptr = DuplicateString(value, ctx->alloc).ptr;
            }
        } else {
            discount_html.blockhtml(ob, text, udata);
        }
    };

    // We need <span> tags around code lines for CSS line numbering
    renderer.blockcode = [](buf *ob, buf *text, void *) {
        if (ob->size) {
            bufputc(ob, '\n');
        }

        BUFPUTSL(ob, "<pre>");
        if (text) {
            size_t start, end = 0;
            for (;;) {
                start = end;
                while (end < text->size && text->data[end] != '\n') {
                    end++;
                }
                if (end == text->size)
                    break;

                BUFPUTSL(ob, "<span class=\"line\">");
                lus_body_escape(ob, text->data + start, end - start);
                BUFPUTSL(ob, "</span>\n");

                end++;
            }
        }
        BUFPUTSL(ob, "</pre>\n");
    };

    // Convert Markdown to HTML
    {
        buf *ob = bufnew(64);
        RG_DEFER { free(ob); };

        markdown(ob, ib, &renderer);
        page->html_buf.reset(ob->data, free);
        page->html = MakeSpan(ob->data, ob->size);
    }

    return true;
}

static bool RenderFullPage(Span<const uint8_t> html, Span<const PageData> pages,
                           Size page_idx, const char *dest_filename)
{
    StreamWriter st(dest_filename);

    const PageData &page = pages[page_idx];

    bool success = PatchFile(html, &st, [&](Span<const char> key, StreamWriter *writer) {
        if (key == "BASE_URL") {
            Print(writer, "<base href=\"/%1\"/>", page.url);
        } else if (key == "TITLE") {
            writer->Write(page.title);
        } else if (key == "LINKS") {
            for (Size i = 0; i < pages.len; i++) {
                const PageData &menu_page = pages[i];

                if (menu_page.menu) {
                    if (i == page_idx) {
                        PrintLn(writer, "<li><a href=\"%1\" class=\"active\">%2</a></li>",
                                        menu_page.url, menu_page.menu);
                    } else {
                        PrintLn(writer, "<li><a href=\"%1\">%2</a></li>",
                                        menu_page.url, menu_page.menu);
                    }
                }
            }
        } else if (key == "TOC") {
            if (page.sections.len) {
                PrintLn(writer,
R"(<a id="side_deploy" href="#" onclick="toggleMenu('#side_menu'); return false;"></a>
<nav id="side_menu">
    <ul>)");

                for (const PageSection &sec: page.sections) {
                    PrintLn(writer, "        <li><a href=\"#%1\" class=\"lv%2\">%3</a></li>",
                                    sec.id, sec.level, sec.title);
                }

                Print(writer,
R"(    </ul>
</nav>)");
            }
        } else if (key == "CONTENT") {
            writer->Write(page.html);
        } else {
            Print(writer, "{%1}", key);
        }
    });

    if (!success)
        return false;
    if (!st.Close())
        return false;

    return true;
}

int RunHodler(int argc, char *argv[])
{
    BlockAllocator temp_alloc;

    // Options
    const char *input_dir = nullptr;
    const char *template_dir = {};
    const char *output_dir = nullptr;
    bool subdirs = false;
    bool ugly_urls = false;

    const auto print_usage = [](FILE *fp) {
        PrintLn(fp, R"(Usage: %!..+%1 <input_dir> -O <output_dir>%!0

Options:
    %!..+-T, --template_dir <dir>%!0     Set template directory

    %!..+-O, --output_dir <dir>%!0       Set output directory

    %!..+-u, --ugly_urls%!0              Add '.html' extension to page URLs
        %!..+--subdirs%!0                Output HTML pages in subdirectories)", FelixTarget);
    };

    // Handle version
    if (argc >= 2 && TestStr(argv[1], "--version")) {
        PrintLn("%!R..%1%!0 %!..+%2%!0", FelixTarget, FelixVersion);
        PrintLn("Compiler: %1", FelixCompiler);
        return 0;
    }

    // Parse options
    {
        OptionParser opt(argc, argv);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else if (opt.Test("-T", "--template_dir", OptionType::Value)) {
                template_dir = opt.current_value;
            } else if (opt.Test("-O", "--output_dir", OptionType::Value)) {
                output_dir = opt.current_value;
            } else if (opt.Test("-u", "--ugly_urls")) {
                ugly_urls = true;
            } else if (opt.Test("--subdirs")) {
                subdirs = true;
            } else {
                LogError("Cannot handle option '%1'", opt.current_option);
                return 1;
            }
        }

        input_dir = opt.ConsumeNonOption();
    }

    // Check arguments
    {
        bool valid = true;

        if (!input_dir) {
            LogError("Missing input directory");
            valid = false;
        }
        if (!output_dir) {
            LogError("Missing output directory");
            valid = false;
        }

        if (!valid)
            return 1;
    }

    if (!template_dir) {
        const char *directory = Fmt(&temp_alloc, "%1%/template", input_dir).ptr;

        if (!TestFile(directory, FileType::Directory)) {
            LogError("Missing template directory");
            return 1;
        }

        template_dir = directory;
    }

    // List input files
    HeapArray<const char *> filenames;
    if (!EnumerateFiles(input_dir, "*.md", 0, 1024, &temp_alloc, &filenames))
        return 1;
    std::sort(filenames.begin(), filenames.end(),
              [](const char *filename1, const char *filename2) { return CmpStr(filename1, filename2) < 0; });

    // Render pages
    HeapArray<PageData> pages;
    {
        HashMap<const char *, Size> pages_map;

        for (const char *filename: filenames) {
            PageData page = {};

            page.src_filename = filename;
            if (!RenderPageContent(&page, &temp_alloc))
                return 1;
            page.name = FileNameToPageName(filename, &temp_alloc);
            if (subdirs) {
                if (TestStr(page.name, "index")) {
                    page.url = "/";
                } else {
                    page.url = Fmt(&temp_alloc, "/%1", page.name).ptr;
                }
            } else if (ugly_urls) {
                page.url = Fmt(&temp_alloc, "%1.html", page.name).ptr;
            } else {
                page.url = page.name;
            }

            bool valid = true;
            if (!page.name) {
                LogError("%1: Page with empty name", page.src_filename);
                valid = false;
            }
            if (!page.title) {
                LogError("%1: Ignoring page without title", page.src_filename);
                valid = false;
            }
            if (!page.created) {
                LogError("%1: Missing creation date", page.src_filename);
            }
            if (Size prev_idx = pages_map.FindValue(page.name, -1); prev_idx >= 0) {
                LogError("%1: Ignoring duplicate of '%2'",
                         page.src_filename, pages[prev_idx].src_filename);
                valid = false;
            }

            if (valid) {
                pages_map.Set(page.name, pages.len);
                pages.Append(page);
            }
        }
    }

    // Output directory
    if (!MakeDirectory(output_dir, false))
        return 1;
    LogInfo("Template: %!..+%1%!0", template_dir);
    LogInfo("Output directory: %!..+%1%!0", output_dir);

    const char *template_filename = Fmt(&temp_alloc, "%1%/page.html", template_dir).ptr;
    const char *static_directory = Fmt(&temp_alloc, "%1%/static", template_dir).ptr;

    HeapArray<uint8_t> template_html;
    if (!ReadFile(template_filename, Mebibytes(1), &template_html))
        return 1;

    // Output fully-formed pages
    for (Size i = 0; i < pages.len; i++) {
        const PageData &page = pages[i];

        const char *dest_filename;
        if (subdirs && !TestStr(pages[i].name, "index")) {
            dest_filename = Fmt(&temp_alloc, "%1%/%2%/index.html", output_dir, page.name).ptr;
            if (!EnsureDirectoryExists(dest_filename))
                return 1;
        } else {
            dest_filename = Fmt(&temp_alloc, "%1%/%2.html", output_dir, page.name).ptr;
        }

        if (!RenderFullPage(template_html, pages, i, dest_filename))
            return 1;
    }

    // Copy template assets
    if (TestFile(static_directory, FileType::Directory)) {
        HeapArray<const char *> template_files;
        if (!EnumerateFiles(static_directory, nullptr, 3, 1024, &temp_alloc, &template_files))
            return 1;

        Size prefix_len = strlen(static_directory);

        for (const char *src_filename: template_files) {
            const char *basename = TrimStrLeft(src_filename + prefix_len, RG_PATH_SEPARATORS).ptr;

            if (TestStr(basename, "page.html"))
                continue;

            const char *dest_filename = Fmt(&temp_alloc, "%1%/static%/%2", output_dir, basename).ptr;

            if (!EnsureDirectoryExists(dest_filename))
                return 1;

            StreamReader reader(src_filename);
            StreamWriter writer(dest_filename);
            if (!SpliceStream(&reader, Megabytes(4), &writer))
                return 1;
            if (!writer.Close())
                return 1;
        }
    }

    LogInfo("Done!");
    return 0;
}

}

// C++ namespaces are stupid
int main(int argc, char **argv) { return RG::RunHodler(argc, argv); }