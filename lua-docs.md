# This is AI generated
## CopyPasta Lua API Reference

A C++ code-refactoring toolkit exposed to Lua via LuaBridge. The API provides
Tree-sitter CST parsing, queued/validated file edits, file IO, directory
walking, and libgit2 integration.

All classes are instantiated through global factory functions (`read`, `write`,
`loadLanguage`, `gitOpen`, …) or direct constructors (`Editor()`, `File(path)`).
Lua tables returned by the API are **1-indexed**.

---

## Table of Contents

- [Capture Tables](#capture-tables)
- [Global Functions](#global-functions)
- [File](#file)
- [FileReader](#filereader)
- [frCache (FileReader cache)](#frcache-namespace)
- [FileWriter](#filewriter)
- [Language](#language)
- [Tree](#tree)
- [Editor](#editor)
- [Git](#git)
- [Logger](#logger)
- [Helper.Table](#helpertable)
- [Helper.String](#helperstring)
- [Globals: `cmdArgs`](#globals)
- [End-to-End Examples](#end-to-end-examples)
- [Performance Guide](#performance-guide)
- [Real-World Recipes](#real-world-recipes)

---

## Capture Tables

Several methods produce or consume **capture tables** — plain Lua tables
describing a byte/point range in a source file. They are the common currency
between `Tree:query`, `FileReader:find`, and `Editor` operations.

A capture has the shape:

| Field       | Type     | Meaning                          |
|-------------|----------|----------------------------------|
| `startByte` | integer  | Start byte offset                |
| `endByte`   | integer  | End byte offset (exclusive)      |
| `row`       | integer  | Start row (0-based)              |
| `col`       | integer  | Start column (0-based)           |
| `endRow`    | integer  | End row                          |
| `endCol`    | integer  | End column                       |
| `text`      | string   | Source text of the range         |
| `name`      | string   | Capture name (query captures)    |

`Editor` operations additionally read optional fields off the same table:
`change` (string), `context` (string), `id`. These are passed through and used
for error reporting / review metadata. You may attach them to a capture before
queuing an edit.

---

## Global Functions

| Function                              | Returns      | Description                                              |
|---------------------------------------|--------------|----------------------------------------------------------|
| `read(path: string)`                  | `FileReader` | Open a file for reading.                                 |
| `readSnap(cont: string)`              | `FileReader` | Reader over an in-memory string snapshot.                |
| `write(path: string)`                 | `FileWriter` | Open a file for writing.                                 |
| `walk(path, opts, callback)`          | `nil`        | Recursively walk a directory. See [walk](#walk).         |
| `findInFiles(path, pattern, opts)`    | `table`      | Multithreaded regex search across files. See below.      |
| `loadLanguage(name: string)`          | `Language`   | Load a Tree-sitter grammar by name (e.g. `"java"`).      |
| `gitClone(...)`                       | `Git`        | Clone a repository.                                      |
| `gitOpen(path: string)`               | `Git`        | Open an existing repository.                             |
| `gitOpenOrInit(path: string)`         | `Git`        | Open a repo, initializing it if absent.                  |

### walk

```lua
walk(path: string, opts: table, callback: function)
```

`opts` fields (all read as the indicated type):

| Key                  | Type            | Meaning                                              |
|----------------------|-----------------|------------------------------------------------------|
| `notRecursive`       | bool            | Do not Descend into subdirectories.                  |
| `inverted`           | bool            | Invert the matching behavior.                        |
| `notFilesOnly`       | bool            | Do not Only deliver regular files to the callback.   |
| `doNotObeyGitIgnore` | bool            | If true, ignore `.gitignore` rules.                  |
| `usePool`            | bool            | Use a thread pool (reserved).                        |
| `ext`                | array of string | Extension allowlist, e.g. `{".java", ".cpp"}`.       |
| `ignore`             | array of string | Glob patterns to skip, e.g. `{"**/api/**"}`.         |

`callback(file: File, git: Git)` is invoked per entry. Return an integer control
code to steer the walk: `STOP`, `ABORT`, or `SKIP` (matching `DirWalker`
status enums); any other / no return means continue.

### findInFiles

```lua
findInFiles(path: string, pattern: string, opts: table) -> table
```

Searches `path` recursively (files only) for `pattern`, treated as a PCRE2
regex with `PCRE2_MULTILINE`. `opts` accepts `ext` and `ignore` arrays as in
`walk`. Returns an array; each element is a per-file match table in the same
shape as [`FileReader:find`](#filereaderfind).

---

## File

A filesystem entry (file or directory).

### Constructor

```lua
File(path: string) -> File
```

### Properties (read/write data members)

| Property  | Type    | Description                       |
|-----------|---------|-----------------------------------|
| `path`    | string  | Full path string.                 |
| `name`    | string  | File name.                        |
| `ext`     | string  | Extension.                        |
| `isDir`   | bool    | Is a directory.                   |
| `isReg`   | bool    | Is a regular file.                |
| `isValid` | bool    | Entry exists / is valid.          |
| `size`    | integer | File size.                        |
| `level`   | integer | Depth level within a walk.        |

### Methods

| Method                                 | Returns | Description                                  |
|----------------------------------------|---------|----------------------------------------------|
| `file:sync()`                          | —       | Refresh metadata from disk.                  |
| `deleteFile(file: File)`               | —       | Delete the file (free function).             |
| `deleteDir(file: File)`                | —       | Delete the directory (free function).        |
| `rename(file: File, name: string)`     | bool    | Rename; returns success (free function).     |

```lua
local f = File("/tmp/notes.txt")
print(f.name, f.ext, f.size)
f:sync()
rename(f, "/tmp/notes.bak")
```

---

## FileReader

Read-only view over a file's contents with line/offset indexing and search.

### Constructor / factories

```lua
FileReader(path: string) -> FileReader   -- direct
read(path: string)       -> FileReader   -- factory
readSnap(cont: string)   -> FileReader   -- in-memory snapshot
```

### Methods

| Method                                         | Returns | Description                                            |
|------------------------------------------------|---------|--------------------------------------------------------|
| `r:isValid()`                                  | bool    | Whether the file was read successfully.                |
| `r:sync()`                                     | —       | Re-read from disk.                                     |
| `r:get()`                                      | string  | Entire file contents.                                 |
| `r:getRange(from: int, to: int)`               | string  | Substring between byte offsets `[from, to)`.          |
| `r:getLine(row: int)`                          | string  | Text of a single row.                                 |
| `r:getRowStart(row: int)`                      | integer | Byte offset where `row` begins.                        |
| `r:getIndent(row: int)`                        | integer | Indentation of a row.                                 |
| `r:find(pattern: string, regex: bool)`         | table   | Search; `regex=true` for PCRE2 (uses `PCRE2_MULTILINE`). Returns match table. |

The match table returned by `find` is an array of:

| Field      | Type   | Description                                |
|------------|--------|--------------------------------------------|
| `path`     | string | File path.                                 |
| `text`     | string | Matched text.                              |
| `startByte`, `endByte`, `row`, `col`, `endRow`, `endCol` | int | Match range. |
| `captures` | table  | 1-indexed array of capture subtables (each a capture with `text` + range). |

```lua
local r = read("/src/Main.java")
print(r:getLine(0))
local hits = r:find("createQuery\\(", true)
for _, m in ipairs(hits) do
  print(m.path, m.row, m.text)
end
```

---

## frCache (namespace)

A global, shared `FileReader` cache keyed by path — useful for repeated reads.

| Function                                  | Returns | Description                                         |
|-------------------------------------------|---------|-----------------------------------------------------|
| `frCache.getLine(path: string, row: int)` | string  | Cached line read (`""` if file invalid).            |
| `frCache.get(path: string)`               | string  | Cached full contents (`""` if invalid).             |
| `frCache.updateAndGet(path: string)`      | string  | Refresh from disk, then return full contents.       |
| `frCache.invalidate(path: string)`        | —       | Drop one path from the cache.                       |
| `frCache.clear()`                         | —       | Empty the entire cache.                             |

```lua
local line = frCache.getLine("/src/Main.java", 10)
frCache.invalidate("/src/Main.java")
```

---

## FileWriter

Mutating writer over a file. Edits are applied directly; call `save` to persist.

### Constructor / factory

```lua
FileWriter(path: string) -> FileWriter   -- direct
write(path: string)      -> FileWriter   -- factory
```

### Methods

| Method                                              | Returns | Description                                         |
|-----------------------------------------------------|---------|-----------------------------------------------------|
| `w:isValid()`                                       | bool    | Writer is usable.                                   |
| `w:save()`                                          | —       | Persist changes to the original path.               |
| `w:backup(suffix: string)`                          | —       | Write a backup copy with `suffix`.                  |
| `w:writeTo(path: string)`                           | —       | Write current buffer to a different path.           |
| `w:append(...)`                                     | —       | Append content.                                     |
| `w:insert(...)`                                     | —       | Insert content at a position.                       |
| `w:insertRowBefore(...)`                            | —       | Insert a new row before a row.                      |
| `w:insertRowAfter(...)`                             | —       | Insert a new row after a row.                       |
| `w:deleteRow(...)`                                  | —       | Delete a row.                                       |
| `w:deleteCont(...)`                                 | —       | Delete a content range.                             |
| `w:writeAt(offset: int, str: string)`               | —       | Write `str` at byte `offset`.                       |
| `w:replace(...)`                                    | —       | Replace a range.                                    |
| `w:replaceAll(...)`                                 | —       | Replace all matches.                                |

```lua
local w = write("/src/Main.java")
w:writeAt(0, "// header\n")
w:backup(".bak")
w:save()
```

> For CST-correct, conflict-checked, queued editing prefer [`Editor`](#editor)
> over `FileWriter` direct mutation.

---

## Language

A loaded Tree-sitter grammar. Obtain via `loadLanguage(name)`.

### Methods

| Method                                | Returns | Description                                                   |
|---------------------------------------|---------|---------------------------------------------------------------|
| `lang:parse(input: string)`           | `Tree`  | Parse a source string into a CST. `input` must be non-empty.  |
| `lang:parseFile(path: string)`        | `Tree`  | Read `path` and parse it into a CST.                          |
| `lang:getNodeTypes()`                 | table   | Map of node-type categories → arrays of available node names. |

```lua
local java = loadLanguage("java")
Helper.Table.print(java:getNodeTypes())
local tree = java:parse("class A {}")
```

> Note: the example scripts call `java:parse(file.path, true)`. The single
> required parameter is the source/path; trailing arguments are tolerated by the
> binding. Use `parseFile(path)` when you intend to parse from a file path.

---

## Tree

A parsed CST. Returned by `Language:parse` / `Language:parseFile`.

### Methods

| Method                                          | Returns | Description                                                       |
|-------------------------------------------------|---------|-------------------------------------------------------------------|
| `tree:sexp()`                                   | string  | S-expression dump of the tree.                                   |
| `tree:asQuery()`                                | string  | Render the tree as a query string.                               |
| `tree:getErrors()`                              | table   | Array of capture-range tables for CST error nodes.               |
| `tree:query(expr: string, callback: function)`  | —       | Run a Tree-sitter query; invoke `callback` per match.            |

In `query`, `callback(captures)` receives a table keyed by **capture name**;
each value is a capture table (with `text`, `row`, `col`, byte offsets, etc.).

```lua
local q = [[
  (method_invocation (identifier) @name (#eq? @name "createNativeQuery"))
]]
tree:query(q, function(c)
  print(c.name.text, c.name.row, c.name.col)
end)

-- inspect parse errors
for _, e in ipairs(tree:getErrors()) do
  print("CST error at", e.row, e.col)
end
```

---

## Editor

Queues typed, validated edit operations and applies them against a `Tree` +
`FileWriter`. Edits fail-fast with structured error reporting (conflicts,
CST errors, missing nodes) before / while writing.

### Constructor

```lua
Editor() -> Editor
```

### Queuing operations

Each method takes a **capture table** (`cap`) describing the target range and
queues an operation. Optional `cap.context`, `cap.change`, `cap.id` are carried
through for error/review metadata.

| Method                                            | Operation            | Description                                              |
|---------------------------------------------------|----------------------|----------------------------------------------------------|
| `ed:insertBefore(cap, text: string)`              | INSERT               | Insert `text` immediately before the capture start.      |
| `ed:insertAfter(cap, text: string)`               | INSERT               | Insert `text` immediately after the capture end.         |
| `ed:insertRowBefore(cap)`                         | INSERT_ROW_BEFORE    | Insert a row before; uses `cap.change` as content.       |
| `ed:insertRowAfter(cap)`                          | INSERT_ROW_AFTER     | Insert a row after; uses `cap.change`.                   |
| `ed:write(cap, text: string)`                     | WRITE                | Overwrite the capture range with `text`.                 |
| `ed:replace(cap, pattern: string, tpl: string)`   | REPLACE              | Regex replace within the range (`pattern` → `tpl`).      |
| `ed:delete(cap)`                                  | DELETE               | Delete the capture range.                                |
| `ed:deleteWithPad(cap, pad: int)`                 | DELETE               | Delete range expanded by `pad` bytes each side.          |
| `ed:printBefore(cap)`                             | PRINT_CHANGE_BEFORE  | Diagnostic print of the change before the range.         |
| `ed:printAfter(cap)`                              | PRINT_CHANGE_AFTER   | Diagnostic print of the change after the range.          |
| `ed:mark(cap)`                                    | MARK                 | Mark a range (uses `cap.change` text, `cap.context`).    |
| `ed:validate(cap)`                                | VALIDATE_CST         | Validate CST after preceding edits.                      |
| `ed:printErrors(cap)`                             | PRINT_ERRORS         | Print accumulated errors.                                |
| `ed:backup(cap, suffix: string)`                  | BACKUP               | Queue a backup with `suffix`.                            |
| `ed:writeTo(path: string)`                        | WRITE_TO             | Queue writing output to `path`.                          |
| `ed:queueSave()`                                  | SAVE                 | Queue a save operation (saves to the writer's target). |

### Inspecting / controlling

| Method                  | Returns | Description                                        |
|-------------------------|---------|----------------------------------------------------|
| `ed:getErrors()`        | table   | All errors (see [Error table](#error-table)).      |
| `ed:getConflicts()`     | table   | Conflict errors only.                              |
| `ed:cancel(...)`        | —       | Cancel a queued edit (`delEdit`).                  |
| `ed:reset()`            | —       | Clear the queue.                                   |

### Applying

| Method                                              | Returns | Description                                                       |
|-----------------------------------------------------|---------|-------------------------------------------------------------------|
| `ed:apply(tree: Tree, w: FileWriter)`               | table   | Apply queued edits; return error table.                          |
| `ed:applyAndSave(tree, w)`                          | table   | Apply then save; return errors.                                  |
| `ed:applyAndSaveValidOnly(tree, w)`                 | table   | Apply, save only edits that keep the CST valid; return errors.   |
| `ed:applySaveAndMarkErrors(tree, w)`                | table   | Apply, save, and annotate error sites; return errors.            |
| `ed:step(tree, w)`                                  | table   | Apply a single step (for interactive review); return errors.     |

### Error table

`getErrors`, `getConflicts`, and all `apply*`/`step` calls return an array of:

| Field       | Type    | Description                                              |
|-------------|---------|----------------------------------------------------------|
| `type`      | string  | `"CONFLICT"`, `"CST_ERROR"`, or `"CST_MISSING"`.         |
| `startRow`, `startCol`, `endRow`, `endCol` | int | Error range. |
| `edit`      | table   | Offending edit: `op` (int), `change`, `context`, `id`.   |

```lua
local edt = Editor()
edt:write(cap, "StringUtilities.valueOf(" .. inner .. ")")
local errs = edt:applyAndSaveValidOnly(tree, w)
for _, e in ipairs(errs) do
  print(e.type, e.startRow, e.startCol, e.edit.op)
end
```

---

## Git

libgit2 wrapper. Obtain via `gitOpen`, `gitOpenOrInit`, or `gitClone`.

### Methods

| Method                                              | Returns | Description                                       |
|-----------------------------------------------------|---------|---------------------------------------------------|
| `g:isIgnored(path: string)`                         | bool    | Whether `path` is git-ignored.                    |
| `g:addIgnore(...)`                                  | —       | Add an ignore rule.                               |
| `g:add(path: string)`                               | —       | Stage a path.                                     |
| `g:addAll()`                                        | —       | Stage all changes.                                |
| `g:commit(...)`                                     | —       | Create a commit.                                  |
| `g:resetHead()`                                     | —       | Reset HEAD (unstage).                             |
| `g:setSignature(...)`                               | —       | Set author/committer signature.                   |
| `g:branchExists(name: string)`                      | bool    | Whether a branch exists.                          |
| `g:branchCreate(name: string)`                      | —       | Create a branch.                                  |
| `g:checkout(blobId: string)`                        | —       | Checkout a branch / ref / blob.                   |
| `g:diff()`                                          | table   | Working-tree diff (array of FileDiff).            |
| `g:diffFromTo(from: string, to: string)`            | table   | Diff between two refs.                             |

### Diff structures

**FileDiff:** `oldPath`, `newPath` (string), `status`, `flags` (int),
`hunks` (array of Hunk).

**Hunk:** `oldStart`, `oldLines`, `newStart`, `newLines` (int), `header`
(string), `lines` (array of LineDiff).

**LineDiff:** `type` (single-char string), `oldLineNo`, `newLineNo`,
`fileOffset` (int), `text`, `author`, `email`, `commit` (string).

```lua
local Git = gitOpen(path)
Git:resetHead()
if not Git:branchExists("test-b-1") then Git:branchCreate("test-b-1") end
Git:checkout("test-b-1")

for _, fd in ipairs(Git:diff()) do
  print(fd.oldPath, "->", fd.newPath)
  for _, h in ipairs(fd.hunks) do
    print(h.header)
    for _, ln in ipairs(h.lines) do
      print(ln.type, ln.newLineNo, ln.text)
    end
  end
end
```

---

## Logger

| Member / Function               | Description                                  |
|---------------------------------|----------------------------------------------|
| `Logger.level`                  | Read/write log verbosity level (integer).    |
| `Logger.info(msg: string)`      | Log at info level.                           |
| `Logger.error(msg: string)`     | Log at error level.                          |
| `Logger.debug(msg: string)`     | Log at debug level.                          |
| `Logger.debug_full(msg: string)`| Verbose debug log.                           |

```lua
Logger.level = 3
Logger.info("starting refactor")
```

---

## Helper.Table

| Function                          | Returns | Description                                                    |
|-----------------------------------|---------|----------------------------------------------------------------|
| `Helper.Table.print(t: table)`    | —       | Recursively print a table as `key.path = value` lines.         |
| `Helper.Table.keys(t: table)`     | table   | Array of top-level (`depth == 0`) key paths.                   |
| `Helper.Table.values(t: table)`   | table   | Array of top-level values (as strings).                        |

```lua
Helper.Table.print(java:getNodeTypes())
local ks = Helper.Table.keys(someTable)
```

---

## Helper.String

| Function                                                | Returns | Description                                  |
|---------------------------------------------------------|---------|----------------------------------------------|
| `Helper.String.startsWith(s, prefix)`                   | bool    | Prefix test.                                 |
| `Helper.String.endsWith(s, suffix)`                     | bool    | Suffix test.                                 |
| `Helper.String.contains(s, sub)`                        | bool    | Substring test.                              |
| `Helper.String.trim(s)`                                 | string  | Trim whitespace both ends.                   |
| `Helper.String.ltrim(s)`                                | string  | Trim left.                                   |
| `Helper.String.rtrim(s)`                                | string  | Trim right.                                  |
| `Helper.String.split(s, delim)`                         | table   | Split into array on `delim`.                 |
| `Helper.String.repeat(s, n)`                            | string  | Repeat `s` `n` times.                        |
| `Helper.String.padLeft(s, totalWidth, pad)`             | string  | Left-pad to width with `pad`.                |
| `Helper.String.padRight(s, totalWidth, pad)`            | string  | Right-pad to width.                          |
| `Helper.String.count(s, sub)`                           | integer | Count non-overlapping occurrences.           |
| `Helper.String.isEmpty(s)`                              | bool    | Length-zero test.                            |
| `Helper.String.isBlank(s)`                              | bool    | Whitespace-only test.                        |
| `Helper.String.reverse(s)`                              | string  | Reverse the string.                          |

```lua
local parts = Helper.String.split("a,b,c", ",")   -- {"a","b","c"}
print(Helper.String.padLeft("7", 3, "0"))         -- "007"
```

---

## Globals

| Global    | Type  | Description                                                |
|-----------|-------|------------------------------------------------------------|
| `cmdArgs` | table | 1-indexed array of command-line arguments passed to the executor. |

```lua
for i, a in ipairs(cmdArgs) do print(i, a) end
```

---

## End-to-End Examples

### 1. Find call sites across a project

```lua
path = io.read()
local java = loadLanguage("java")
print("find createQuery instances")

local hits = findInFiles(
  path,
  [[createQuery\(\s*.*,\s*Object\[\]\.class\s*\)]],
  { ext = { ".java" } }
)

local i = 1
for _, h in ipairs(hits) do
  for _, m in ipairs(h) do
    print(m.path .. ":" .. m.row .. ":" .. m.col)
    print(m.text)
    print(i)
    i = i + 1
  end
end
```

### 2. CST-driven refactor with git branch + validated save

```lua
print("staticCastHandling")
Logger.level = 3
local java = loadLanguage("java")

local findNativeQuery = [[
  (method_invocation (identifier) @method_name (#eq? @method_name "createNativeQuery")
    arguments: (argument_list
      [(string_literal) (binary_expression)] @first_arg
      (class_literal) @second_arg (#eq? @second_arg "Object[].class")))
]]

local findCasts = [[
  (cast_expression type: (type_identifier) @cast_type value: (array_access) @cast_value) @cast
]]

local wrappers = {
  Character = "StringUtilities.valueOf(",
  String    = "StringUtilities.valueOf(",
  Long      = "NumberUtilities.toLong(",
  Integer   = "NumberUtilities.toInteger(",
}

path = io.read()

local Git = gitOpen(path)
if not Git.isValid then print("Unable to load Git repo") return end
Git:resetHead()
local branch = "test-b-1"
if not Git:branchExists(branch) then Git:branchCreate(branch) end
Git:checkout(branch)

walk(path,
  { ext = { ".java" },
    ignore = { "**/hibernate/**", "**/datatypes/**", "**/api/**" },
  },
  function(file, git)
    print(file.path)
    if file.path == "" then return end

    local r    = read(file.path)
    local tree = java:parse(file.path, true)
    local w    = write(file.path)
    local edt  = Editor()

    tree:query(findNativeQuery, function(c)
      if c.second_arg.text ~= "Object[].class" then return end
      tree:query(findCasts, function(match)
        if match.cast.row <= c.second_arg.row then return end
        if match.cast.row >  c.second_arg.row + 200 then return end
        local wrapper = wrappers[match.cast_type.text]
        if not wrapper then return end
        local repl = wrapper .. match.cast_value.text .. ")"
        edt:write(match.cast, repl)
      end)
    end)

    edt:applyAndSaveValidOnly(tree, w)
    git:add(file.path)
  end)
```

### 3. Inspect available node types for a grammar

```lua
local java = loadLanguage("java")
Helper.Table.print(java:getNodeTypes())
```

### 4. Insert imports before the first match, with error reporting

```lua
local java = loadLanguage("java")
local tree = java:parseFile("/src/Service.java")
local w    = write("/src/Service.java")
local edt  = Editor()

tree:query("(import_declaration) @imp", function(c)
  edt:insertBefore(c.imp, "import com.example.StringUtilities;\n")
end)

local errs = edt:applyAndSave(tree, w)
for _, e in ipairs(errs) do
  print(e.type, e.startRow, e.startCol)
end
```

### 5. Interactive stepping (for human review UI)

```lua
local tree = java:parseFile(file.path)
local w    = write(file.path)
local edt  = Editor()

-- queue several operations
tree:query("(method_declaration) @m", function(c)
  edt:printBefore(c.m)        -- diagnostic
  edt:mark(c.m)               -- mark for review
end)

-- apply one step at a time
local errs = edt:step(tree, w)
while #errs == 0 do
  errs = edt:step(tree, w)
end
```

### 6. Directory walk with control flow

```lua
walk("/project",
  { ext = { ".cpp", ".hpp" }, notRecursive = true, notFilesOnly = false },
  function(file, git)
    if Helper.String.contains(file.path, "/third_party/") then
      return 3  -- SKIP
    end
    print(file.path, file.size)
  end)
```

### 7. Cached reads + string helpers

```lua
local content = frCache.get("/src/Main.java")
if Helper.String.contains(content, "deprecated") then
  local first = frCache.getLine("/src/Main.java", 0)
  print("first line:", Helper.String.trim(first))
end
frCache.invalidate("/src/Main.java")
```

### 8. Reading from an in-memory snapshot (`readSnap`)

```lua
-- Build a reader over a string instead of a file on disk.
local src = [[
class A {
  void f() { createNativeQuery(q, Object[].class); }
}
]]

local r = readSnap(src)
if r:isValid() then
  print(r:getLine(1))           -- "class A {"
  local hits = r:find("createNativeQuery", true)
  for _, m in ipairs(hits) do
    print(m.row, m.col, m.text)
  end
end
```

`readSnap` builds a reader over an in-memory `FileSnapshot` rather than a file
on disk — useful for parsing/searching generated or transient source without
touching the filesystem.

### 9. Git diff walk

```lua
local Git = gitOpenOrInit(path)
for _, fd in ipairs(Git:diff()) do
  print("file:", fd.newPath, "status:", fd.status)
  for _, h in ipairs(fd.hunks) do
    print("  " .. h.header)
    for _, ln in ipairs(h.lines) do
      print(string.format("  %s %d: %s", ln.type, ln.newLineNo, ln.text))
    end
  end
end
```

---

## Performance Guide

The toolkit parses CSTs, runs regex over whole files, and walks large trees.
On a big repository the difference between a naive and a tuned script is often
10–50x. The rules below are ordered by impact.

### 1. Narrow the walk before you read anything

Filtering at the `walk`/`findInFiles` level is far cheaper than opening every
file and bailing inside the callback. Use `ext` to skip non-target files and
`ignore` globs to prune whole subtrees (generated code, vendored deps, build
output) so the walker never even stats them.

```lua
walk(path, {
  ext       = { ".java" },
  ignore    = { "**/target/**", "**/generated/**", "**/test/**" },
  notRecursive = false,
  notFilesOnly = false,
}, function(file, git)
  -- only real source files reach here
end)
```

### 2. Use `findInFiles` to locate, then parse only the hits

Parsing every file to run a query is wasteful when only a small fraction
contain the construct you care about. `findInFiles` is multithreaded and runs a
PCRE2 pre-filter across the repo; parse the CST only for files that matched.

```lua
-- cheap, parallel pre-filter
local hits = findInFiles(path, [[createNativeQuery\s*\(]], { ext = { ".java" } })

-- collect unique paths
local seen, targets = {}, {}
for _, h in ipairs(hits) do
  for _, m in ipairs(h) do
    if not seen[m.path] then seen[m.path] = true; targets[#targets+1] = m.path end
  end
end

-- expensive, precise CST pass only where needed
local java = loadLanguage("java")
for _, p in ipairs(targets) do
  local tree = java:parseFile(p)
  tree:query(myQuery, function(c) --[[ ... ]] end)
end
```

### 3. Let the query engine do the filtering, not Lua

Every match that crosses the C++→Lua boundary costs a table allocation and a
callback dispatch. Push predicates (`#eq?`, `#match?`) and structure into the
query so the engine rejects non-matches before they ever reach your callback.
A precise query with `#eq?` constraints beats a broad query plus an `if` guard
in Lua.

```lua
-- GOOD: engine filters; callback only fires for real hits
local q = [[
  (method_invocation
    name: (identifier) @m (#eq? @m "createNativeQuery")
    arguments: (argument_list (class_literal) @cls (#eq? @cls "Object[].class")))
]]

-- AVOID: broad query, then reject most matches in Lua
-- (method_invocation name: (identifier) @m)  -> if c.m.text ~= "createNativeQuery" then return end
```

### 4. Reuse readers via `frCache`

When the same files are read repeatedly across passes (e.g. a locate pass then
a verify pass), `frCache` avoids re-reading and re-indexing. Invalidate a path
after you write to it so later reads see fresh content.

```lua
local content = frCache.get(file.path)
-- ... edit and save via Editor ...
frCache.invalidate(file.path)   -- force re-read next time
```

### 5. Query caches are automatic — reuse query *strings*

`Tree:query` compiles through `TSQueryCache` keyed by the query string, so
hoisting your query literals to module scope (rather than rebuilding them with
string concatenation each call) lets the cache hit. Keep query text constant.

```lua
-- module scope: compiled once, cached
local QUERY = [[ (method_declaration name: (identifier) @n) ]]

walk(path, opts, function(file)
  local tree = java:parseFile(file.path)
  tree:query(QUERY, handler)   -- cache hit every iteration
end)
```

### 6. Batch edits per file, apply once

`Editor` is designed to accumulate a queue and apply atomically. Queue all
edits for a file from every query callback, then call a single
`applyAndSaveValidOnly` — don't open a new `Editor`/`FileWriter` per match.
Applying once also lets the editor detect conflicts between overlapping edits
before writing.

```lua
local edt = Editor()
local w   = write(file.path)

tree:query(Q1, function(c) edt:write(c.target, repl(c)) end)
tree:query(Q2, function(c) edt:insertBefore(c.anchor, header) end)

-- one apply, one save, conflict-checked
local errs = edt:applyAndSaveValidOnly(tree, w)
```

### 7. Prefer `applyAndSaveValidOnly` on bulk runs

On a repo-wide refactor you usually want to commit only the edits that keep the
file's CST valid and skip the rest, rather than aborting the whole run on one
bad transformation. `applyAndSaveValidOnly` does this in a single pass and
returns the errors for the skipped edits so you can log and review them.

### 8. Keep log level low in hot loops

`Logger.level = 3` (debug-full) inside a per-file or per-match loop can dominate
runtime. Raise verbosity only while diagnosing; run bulk jobs quiet.

---

## Real-World Recipes

End-to-end scripts for common engineering tasks. Each is self-contained and
shows the locate → parse → transform → save → stage pipeline.

### A. Repo-wide audit report (search + doc generation)

Scan a Java codebase for risky raw-SQL call sites and emit a Markdown report —
no code is modified. Demonstrates `findInFiles` for the cheap pass and CST
confirmation for precision.

```lua
path = io.read()
local java = loadLanguage("java")

-- 1. cheap parallel locate
local hits = findInFiles(path,
  [[createNativeQuery\s*\(]],
  { ext = { ".java" }, ignore = { "**/test/**" } })

-- 2. confirm with a CST query and collect structured findings
local CONFIRM = [[
  (method_invocation
    name: (identifier) @m (#eq? @m "createNativeQuery")
    arguments: (argument_list (_) @firstArg . (class_literal)? @cast)) @call
]]

local report = { "# Native Query Audit\n" }
local total = 0

for _, h in ipairs(hits) do
  for _, m in ipairs(h) do
    local tree = java:parseFile(m.path)
    tree:query(CONFIRM, function(c)
      total = total + 1
      local castTxt = c.cast and c.cast.text or "(none)"
      report[#report+1] = string.format(
        "- `%s:%d` — cast: `%s`\n    ```java\n    %s\n    ```",
        m.path, c.call.row + 1, castTxt,
        Helper.String.trim(c.call.text))
    end)
  end
end

report[#report+1] = string.format("\n**Total call sites: %d**\n", total)

local out = write("/tmp/native_query_audit.md")
out:writeAt(0, table.concat(report, "\n"))
out:save()
print("wrote audit with " .. total .. " findings")
```

### B. Safe API migration on a branch (search + refactor + git)

Wrap unsafe array-cast results from `createNativeQuery(..., Object[].class)` in
typed utility calls, but only on a dedicated branch, committing per file, and
saving only edits that keep the tree valid. This is the production version of
the static-cast example.

```lua
Logger.level = 1                       -- quiet for bulk
local java = loadLanguage("java")
path = io.read()

local FIND_NATIVE = [[
  (method_invocation
    name: (identifier) @m (#eq? @m "createNativeQuery")
    arguments: (argument_list
      [(string_literal) (binary_expression)] @first
      (class_literal) @cls (#eq? @cls "Object[].class"))) @call
]]

local FIND_CAST = [[
  (cast_expression
    type:  (type_identifier) @castType
    value: (array_access)    @castValue) @cast
]]

local WRAP = {
  Character = "StringUtilities.valueOf(",
  String    = "StringUtilities.valueOf(",
  Long      = "NumberUtilities.toLong(",
  Integer   = "NumberUtilities.toInteger(",
}

local Git = gitOpen(path)
Git:resetHead()
local branch = "refactor/native-query-casts"
if not Git:branchExists(branch) then Git:branchCreate(branch) end
Git:checkout(branch)

local changed = 0

walk(path,
  { ext = { ".java" },
    ignore = { "**/hibernate/**", "**/datatypes/**", "**/api/**", "**/test/**" },
    notRecursive = false, notFilesOnly = false },
  function(file, git)
    if file.path == "" then return end

    local tree = java:parseFile(file.path)
    local w    = write(file.path)
    local edt  = Editor()
    local queued = false

    tree:query(FIND_NATIVE, function(call)
      -- only casts that appear after the call, within a window
      tree:query(FIND_CAST, function(match)
        if match.cast.row <= call.cls.row then return end
        if match.cast.row >  call.cls.row + 200 then return end
        local wrapper = WRAP[match.castType.text]
        if not wrapper then return end
        edt:write(match.cast, wrapper .. match.castValue.text .. ")")
        queued = true
      end)
    end)

    if queued then
      local errs = edt:applyAndSaveValidOnly(tree, w)
      git:add(file.path)
      changed = changed + 1
      if #errs > 0 then
        Logger.error(file.path .. ": " .. #errs .. " edits skipped (invalid CST)")
      end
    end
  end)

Git:add(".")
Git:commit("Wrap Object[].class native-query casts in typed utilities")
print("modified " .. changed .. " files on " .. branch)
```

### C. Add missing license/Javadoc headers (doc generation)

Insert a header comment above the first top-level declaration of every source
file that lacks one. Uses a structural query to find the first type declaration
and `insertBefore` to place the block.

```lua
local java = loadLanguage("java")
path = io.read()

local HEADER = [[/*
 * Copyright (c) 2026 Cerillion. All rights reserved.
 */
]]

-- first top-level type declaration in the file
local FIRST_TYPE = [[
  (program
    (_)*
    . [(class_declaration) (interface_declaration) (enum_declaration)] @type)
]]

walk(path, { ext = { ".java" }, notRecursive = false, notFilesOnly = false },
  function(file, git)
    local src = frCache.get(file.path)
    if Helper.String.contains(src, "Copyright (c) 2026 Cerillion") then
      return                                    -- already has header
    end

    local tree = java:parseFile(file.path)
    local w    = write(file.path)
    local edt  = Editor()
    local done = false

    tree:query(FIRST_TYPE, function(c)
      if done then return end                   -- only the first match
      edt:insertBefore(c.type, HEADER)
      done = true
    end)

    if done then
      edt:applyAndSave(tree, w)
      frCache.invalidate(file.path)
      git:add(file.path)
    end
  end)
```

### D. Generate an API surface index (complex query + doc output)

Walk a C++ codebase, extract every public method signature with its enclosing
class, and emit a Markdown reference. Demonstrates nested captures and using
capture ranges to pull exact source text.

```lua
local cpp = loadLanguage("cpp")
path = io.read()

-- class name + each public method declarator
local SURFACE = [[
  (class_specifier
    name: (type_identifier) @class
    body: (field_declaration_list
      (access_specifier) @acc
      (field_declaration
        declarator: (function_declarator
          declarator: (field_identifier) @method)) @decl))
]]

local docs = { "# API Surface\n" }

walk(path, { ext = { ".hpp", ".h" }, notRecursive = false, notFilesOnly = not },
  function(file, git)
    local tree = cpp:parseFile(file.path)
    local current = nil
    tree:query(SURFACE, function(c)
      if c.acc.text ~= "public" then return end
      if c.class.text ~= current then
        current = c.class.text
        docs[#docs+1] = "\n## " .. current .. "\n"
      end
      docs[#docs+1] = "- `" .. Helper.String.trim(c.decl.text) .. "`"
    end)
  end)

local out = write("/tmp/api_surface.md")
out:writeAt(0, table.concat(docs, "\n"))
out:save()
print("API index written, " .. (#docs - 1) .. " entries")
```

### E. Rename a method across a codebase (complex query + multi-edit)

Rename calls to a deprecated method, handling both the invocation and any
import/qualified references. Shows multiple queries feeding one `Editor` queue
per file and conflict-checked application.

```lua
local java = loadLanguage("java")
path = io.read()

local OLD, NEW = "fetchAll", "findAll"

-- 1. unqualified and qualified invocations of the old name
local CALLS = string.format([[
  (method_invocation name: (identifier) @name (#eq? @name "%s")) @call
]], OLD)

-- 2. method *declaration* of the old name (the definition site)
local DECL = string.format([[
  (method_declaration name: (identifier) @name (#eq? @name "%s"))
]], OLD)

local Git = gitOpen(path)
local branch = "refactor/rename-" .. OLD
if not Git:branchExists(branch) then Git:branchCreate(branch) end
Git:checkout(branch)

walk(path, { ext = { ".java" } },
  function(file, git)
    local tree = java:parseFile(file.path)
    local w    = write(file.path)
    local edt  = Editor()
    local n    = 0

    local function rename(cap)
      -- overwrite just the identifier capture, not the whole call
      edt:write(cap.name, NEW)
      n = n + 1
    end

    tree:query(CALLS, function(c) rename(c) end)
    tree:query(DECL,  function(c) rename(c) end)

    if n > 0 then
      local errs = edt:applyAndSaveValidOnly(tree, w)
      git:add(file.path)
      Logger.info(string.format("%s: renamed %d occurrence(s)", file.path, n))
      for _, e in ipairs(errs) do
        Logger.error("  skipped " .. e.type .. " @ " .. e.startRow)
      end
    end
  end)

Git:add(".")
Git:commit(string.format("Rename %s -> %s", OLD, NEW))
```

### F. Enforce a lint rule and mark violations (complex query + review)

Find empty `catch` blocks (a common anti-pattern), and instead of editing
silently, `mark` each one and write a TODO so a human reviews. Uses
`applySaveAndMarkErrors`-style review flow with `mark` + `printErrors`.

```lua
local java = loadLanguage("java")
path = io.read()

-- catch clause whose body block has no statements
local EMPTY_CATCH = [[
  (catch_clause
    body: (block) @body (#eq? @body "{}")) @catch
]]

local violations = 0

walk(path, { ext = { ".java" } },
  function(file, git)
    local tree = java:parseFile(file.path)
    local w    = write(file.path)
    local edt  = Editor()
    local hit  = false

    tree:query(EMPTY_CATCH, function(c)
      c.change  = "// TODO: handle or log this exception\n"
      c.context = "lint:empty-catch"
      edt:insertBefore(c.catch, c.change)
      edt:mark(c)
      violations = violations + 1
      hit = true
      Logger.error(string.format("empty catch at %s:%d", file.path, c.catch.row + 1))
    end)

    if hit then
      edt:applyAndSave(tree, w)
      git:add(file.path)
    end
  end)

print("flagged " .. violations .. " empty catch blocks")
```

### G. Interactive, reviewable refactor (step-through)

For high-risk changes, queue everything but apply one operation at a time via
`step`, inspecting the returned error table between steps. This is the backing
pattern for a human-in-the-loop review UI.

```lua
local java = loadLanguage("java")
local file = cmdArgs[1]

local tree = java:parseFile(file)
local w    = write(file)
local edt  = Editor()

tree:query("(method_declaration) @m", function(c)
  edt:printBefore(c.m)
  edt:write(c.m, annotate(c.m.text))   -- your transform
  edt:validate(c)                      -- check CST validity after each
end)

-- apply incrementally; stop on the first error for inspection
while true do
  local errs = edt:step(tree, w)
  if #errs > 0 then
    for _, e in ipairs(errs) do
      print("PAUSED:", e.type, e.startRow, e.startCol, "op=" .. e.edit.op)
    end
    break
  end
  -- (a UI would await user confirmation here before the next step)
end
```
