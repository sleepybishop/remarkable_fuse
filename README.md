# remarkable_fuse
[![CI](https://github.com/sleepybishop/remarkable_fuse/actions/workflows/ci.yml/badge.svg)](https://github.com/sleepybishop/remarkable_fuse/actions/workflows/ci.yml)

A FUSE filesystem for reMarkable tablets using version 2.x or 3.x of their software (`xochitl`).

This allows you to backup the reMarkable while still retaining access to your documents without requiring the use of USB or cloud services. It's also helpful for running external tools like handwriting recognition on your documents. 

### Features
 - Provides a tree view of documents as they appear in the interface.
 - Notebooks and PDF/EPUB annotations are exposed as folders.
 - Auto-conversion of `.rm` files into **SVG, PNG, and PDF** formats.
 - Page backgrounds (grids, lined paper) are automatically embedded into the exported SVG, PNG, and PDF files using the `templates/` directory.
 - Auto-landscape rotation on all exported files.

### Screenshot
![remfs.png](remfs.png)

### Dependencies
You will need FUSE and libpng development packages to compile the project.
```bash
sudo apt-get install libfuse-dev libpng-dev
```

### Usage
1. Sync a copy of the `xochitl` data folder over ssh to the local dir.
   ```bash
   scp -r root@remarkable_addr:.local/share/remarkable/xochitl .
   ```
2. Create a `config.json` file in the project root:
   ```json
   {
       "data_dir": "/absolute/path/to/xochitl",
       "template_dir": "/absolute/path/to/templates",
       "renderers": ["svg", "png", "pdf"]
   }
   ```
   *(See `config.json.example` for details. Use absolute paths to ensure the FUSE daemon resolves them correctly).*
3. Build the project
   ```bash
   make
   ```
4. Create a directory to be used as a mount point.
   ```bash
   mkdir fused
   ```
5. Mount the filesystem.
   ```bash
   ./remfs --config=config.json fused
   ```
   *(Note: you can unmount later using `fusermount3 -u fused` or `fusermount -u fused`)*

### Command Line Utility
You can also use the standalone `remfmt` binary to convert individual `.rm` files into other formats without mounting the filesystem:
```bash
./remfmt --template-dir templates --template-name "P Grid small" input.rm pdf > output.pdf
```

### TODO
 - Delete support
 - PDF/EPUB/`.rm` imports
 - Typed text input parsing and rendering (v6 scene graph)

### Architecture & Performance TODOs
 - Build a live-sync watcher (`inotify`/`kqueue`) to update the filesystem map without remounting
