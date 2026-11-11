# libclip

libclip is a small C library for reading and writing **CLIP STUDIO FORMAT
(`.clip`)** files, the native document format of Clip Studio Paint.

* Plain C99. The only dependency, SQLite, is bundled as source.
* Reads the canvas, the layer tree, layer properties and full-resolution
  layer pixels from `.clip` files saved by Clip Studio Paint.
* Writes new `.clip` documents from RGBA layers. Clip Studio Paint 5.1 opens
  them.
* Builds as a static or shared library on Windows, Linux and macOS, and can
  be used from C, C++ or any language with a C FFI.

libclip is an independent implementation. It is not made by or affiliated
with CELSYS, and it contains no Clip Studio Paint code. The format was
worked out from files and documented in [`docs/FORMAT.md`](docs/FORMAT.md).
"Clip Studio Paint" and "CLIP STUDIO FORMAT" are trademarks of CELSYS, Inc.

## What it supports

| | |
|---|---|
| **Read** | Canvas size, resolution and colour depth; the layer tree (folders, nesting, palette order); name, visibility, lock, opacity, blend mode, clipping and offsets of each layer; raster layer pixels and layer masks; the embedded preview image; the Paper layer colour; read-only SQL over the embedded database for anything else |
| **Write** | 8-bit RGBA raster layers with name, visibility, lock, clipping, opacity and blend mode; an optional Paper layer; canvas size and DPI; the embedded preview image |
| **Not supported** | Vector, text, gradient, tone, adjustment and 3D layers (they read as plain layer records without pixels); 16-bit and CMYK documents; animation and multi-page projects (`.cmc`); writing folders or masks |
| **Compositing** | libclip returns each layer separately with its blend mode and opacity. Flattening the layers is left to the application. |

The format details behind this, including the rules Clip Studio Paint
checks when it opens a file, are in [`docs/FORMAT.md`](docs/FORMAT.md).

## Adding libclip to a project

### As a git submodule with CMake (recommended)

```sh
git submodule add <libclip repository URL> external/libclip
```

```cmake
add_subdirectory(external/libclip)
target_link_libraries(my_app PRIVATE libclip::clip)
```

When libclip is added with `add_subdirectory`, only the library is built.
Its tests, examples and install rules are switched off unless you turn them
on (see the options below).

### With FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(libclip GIT_REPOSITORY <libclip repository URL> GIT_TAG <tag or commit>)
FetchContent_MakeAvailable(libclip)
target_link_libraries(my_app PRIVATE libclip::clip)
```

### Installed, with `find_package`

```sh
cmake -S libclip -B build-libclip -DCMAKE_BUILD_TYPE=Release
cmake --build build-libclip
cmake --install build-libclip --prefix /path/to/prefix
```

```cmake
find_package(libclip 1 REQUIRED)       # set CMAKE_PREFIX_PATH=/path/to/prefix
target_link_libraries(my_app PRIVATE libclip::clip)
```

### Without CMake

Compile the files in `src/` and `third_party/sqlite/sqlite3.c` together with
your program, with `include/`, `src/` and `third_party/sqlite/` on the
include path. Define `CLIP_BUILDING` when compiling libclip's own sources.
The SQLite settings libclip is tested with are in `CMakeLists.txt`
(`SQLITE_THREADSAFE=0`, `SQLITE_OMIT_LOAD_EXTENSION`, …).

### CMake options

| Option | Default | Meaning |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | Build a DLL / shared object instead of a static library. Users of the shared library get `CLIP_SHARED` defined for them by the CMake target. |
| `LIBCLIP_BUILD_TESTS` | on when top level | Build `libclip_tests` and register it with CTest. |
| `LIBCLIP_BUILD_EXAMPLES` | on when top level | Build `clipinfo` and `clipnew`. |
| `LIBCLIP_INSTALL` | on when top level | Generate install rules and the `libclip` CMake package. |
| `LIBCLIP_REFERENCE_FILE` | empty | A `.clip` file saved by Clip Studio Paint. When set, the tests also read it and round-trip it. |

## Using the API

Everything is declared in [`include/libclip/clip.h`](include/libclip/clip.h).

**Conventions**

* Functions that return `int` return nonzero on success and 0 on failure.
  Pass a `clip_error` to get a message; it may be `NULL`.
* Images are `clip_image`: straight (non-premultiplied) alpha, RGBA, 8 bits
  per channel, rows of `width * 4` bytes with no padding.
* Memory that libclip allocates for you is released by the matching
  `*_free` or `*_close` function.
* Paths are UTF-8 on every platform (see [Windows paths](#windows-paths)).

### Reading a document

```c
#include <stdio.h>
#include <libclip/clip.h>

int dump(const char *path) {
    clip_document *doc;
    clip_error err;
    if (!clip_document_open(&doc, path, &err)) {
        fprintf(stderr, "%s\n", err.message);
        return 0;
    }

    const clip_canvas_info *canvas = clip_document_canvas(doc);
    printf("%.0f x %.0f px\n", canvas->width, canvas->height);

    for (size_t i = 0; i < clip_document_layer_count(doc); ++i) {   /* top to bottom */
        const clip_layer_info *layer = clip_document_layer(doc, i);
        if (clip_layer_is_folder(layer) || clip_layer_is_paper(layer))
            continue;

        clip_image pixels = {0};
        if (clip_document_render_layer(doc, layer, &pixels, &err)) {
            /* pixels: canvas-sized RGBA8, already positioned.
               Composite with layer->composite and layer->opacity (0..256). */
        }
        clip_image_free(&pixels);
    }

    clip_document_close(doc);
    return 1;
}
```

* Layers come in the order a layer palette shows them, top to bottom, with
  each folder followed by its contents. `depth` gives the nesting level.
* `composite` holds the blend mode as a `clip_composite` value. `opacity`
  runs from 0 to 256, where 256 is 100 %.
* `clip_document_render_layer_mask()` returns a layer's mask, and
  `clip_document_canvas_preview()` the flattened preview Clip Studio Paint
  stored in the file.
* `clip_document_open_memory()` reads from a buffer instead of a file.
* For anything the structs don't cover, `clip_document_query()` runs
  read-only SQL against the embedded database. The table and column names
  are described in [`docs/FORMAT.md`](docs/FORMAT.md) §4.

### Writing a document

```c
#include <stdio.h>
#include <libclip/clip.h>

/* ink and flattened are clip_images the application has filled in. */
int save(const clip_image *ink, const clip_image *flattened, const char *path) {
    clip_write_layer layer = {0};
    layer.name = "Ink";
    layer.pixels = *ink;               /* placed at (0, 0), cropped to the canvas */
    layer.visible = 1;
    layer.opacity = 256;               /* 0..256 */
    layer.composite = CLIP_COMPOSITE_NORMAL;

    clip_write_document doc = {0};
    doc.width = 1200;
    doc.height = 1600;
    doc.dpi = 350;
    doc.paper_visible = 1;             /* adds a white "Paper" layer */
    doc.paper_color[0] = doc.paper_color[1] = doc.paper_color[2] = 255;
    doc.layers = &layer;               /* bottom to top */
    doc.layer_count = 1;
    doc.preview = flattened;           /* optional; shown in file browsers */

    clip_error err;
    if (!clip_write_path(&doc, path, &err)) {
        fprintf(stderr, "%s\n", err.message);
        return 0;
    }
    return 1;
}
```

* Layers are given **bottom to top** (the reverse of what the reader
  returns).
* libclip doesn't composite, so the preview is the flattened image of your
  own renderer, or `NULL` for none.
* `clip_write_memory()` writes into a `clip_buffer` instead of a file.
* Output is deterministic: the same input always produces the same bytes.

### Lower-level pieces

These are public for tools and debugging. Most programs won't need them.

* `clip_container_*`: the `CSFCHUNK` chunk container.
* `clip_offscreen_*`: the tiled raster format (attribute blob, tile stream,
  encode and decode).
* `clip_zlib_inflate` / `clip_zlib_deflate`, `clip_crc32` / `clip_adler32`,
  and `clip_png_decode` / `clip_png_encode`: the zlib and PNG code the
  format needs. They can be used on their own, so an application that
  links libclip doesn't need a separate zlib or PNG library for simple
  cases.

## Examples

[`examples/`](examples/) has two short programs that use only the public
header:

* `clipinfo <file.clip> [out_dir]` prints the canvas, the layer tree and
  the chunk list. Given a directory, it also writes `preview.png` and one
  `layer_<n>.png` per raster layer.
* `clipnew <out.clip>` creates a two-layer document from scratch, with a
  Multiply layer at 75 % opacity.

They are built when libclip is configured on its own:

```sh
cmake -S libclip -B build-libclip -DCMAKE_BUILD_TYPE=Release
cmake --build build-libclip
build-libclip/clipnew demo.clip
build-libclip/clipinfo demo.clip .
```

## Things to know

### Windows paths

libclip takes UTF-8 paths and converts them to UTF-16 on Windows. Don't
pass the `argv` that `main()` receives on Windows: it is in the ANSI code
page, which turns characters outside it into `?`. Read the command line
with `CommandLineToArgvW` and convert it with
`WideCharToMultiByte(CP_UTF8, …)`, as `examples/clipinfo.c` does, or
convert the `wchar_t` paths your UI gives you the same way.

### Threads

libclip is not thread-safe. The bundled SQLite is built with
`SQLITE_THREADSAFE=0`, so call libclip from one thread at a time.

### SQLite

The bundled SQLite is built as a separate static target,
`libclip_sqlite3`. If your application also links SQLite statically, the
linker sees two copies of the same `sqlite3_*` symbols. Either build libclip
as a shared library (it exports only its own `clip_*` functions), or compile
libclip's sources against your SQLite instead of the bundled one.
`clip_document_db()` exposes libclip's database handle; only use it with the
SQLite build libclip links against.

## Repository layout

```
include/libclip/clip.h   the public API
src/                     the implementation
  clip_container.c         CSFCHUNK container: parse and build
  clip_offscreen.c         tiled raster: attribute blob, tile stream, encode and decode
  clip_document.c          reader: database, canvas, layer tree, layer pixels, SQL access
  clip_writer.c            writer: clip_write_document -> .clip
  clip_zlib.c              zlib inflate and DEFLATE encoder, CRC-32, Adler-32
  clip_png.c               PNG decode and encode
  clip_util.c              buffers, byte order, UTF-16 strings, UTF-8 paths
  csp_schema_data.h        generated schema tables (see below)
gen/gen_csp_schema.py    regenerates csp_schema_data.h
docs/FORMAT.md           the .clip format reference
examples/                clipinfo.c, clipnew.c
tests/test_libclip.c     unit tests
third_party/sqlite/      SQLite amalgamation (public domain)
LICENSE                  MIT License
```

## Development

Run the tests:

```sh
cmake -S libclip -B build-libclip -DCMAKE_BUILD_TYPE=Release
cmake --build build-libclip
ctest --test-dir build-libclip --output-on-failure
```

To also test against a file saved by Clip Studio Paint, configure with
`-DLIBCLIP_REFERENCE_FILE=/path/to/file.clip`.

**Compatibility.** Clip Studio Paint rejects files that break any of the
rules in [`docs/FORMAT.md`](docs/FORMAT.md) §6, and the unit tests can't
check all of them. When you change the writer, compare its output for the
same input before and after: the writer is deterministic, so identical bytes
mean compatibility is unchanged. If the bytes differ, open the new output in
Clip Studio Paint.

**Schema data.** Files written by libclip declare the same database schema
as Clip Studio Paint's own files. `src/csp_schema_data.h` holds that schema
(table definitions and the `ElemScheme` and `ParamScheme` registries) and
nothing else from the source document. To regenerate it from a file saved
by Clip Studio Paint:

```sh
python gen/gen_csp_schema.py reference.clip src/csp_schema_data.h
```

## Version

1.0.0. `clip_version()` and the `CLIP_VERSION_*` macros report it.

## License

libclip is released under the [MIT License](LICENSE). The bundled SQLite in
`third_party/sqlite/` is in the public domain.
