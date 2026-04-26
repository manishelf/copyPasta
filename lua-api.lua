-- ============================================================
-- Lua API (updated to match implemented bindings)
-- ============================================================

-- Lifetime notes (important)
-- - `loadLanguage(name)` returns a `Language` wrapper that owns a `TSLang` handle
-- (see `include/loader.hpp`). `TSLang` holds the dynamic library handle and
-- a `const TSLanguage*`. Closing the library invalidates that pointer.
-- - Bindings currently make `LuaLanguage`, `LuaTree`, and `LuaQuery` own their
-- own `TSLang` / parser resources so Lua objects remain valid even if the
-- original `Language` userdata is garbage-collected.
-- - `LuaLanguage` holds a `TSLang` and a `TSEngine`.
-- - `LuaTree` owns a moved `TSLang`, its own `TSEngine` and the `TSTree*`.
-- - `LuaQuery` owns a moved `TSLang` and the compiled `TSQuery*`.
-- - `TSEngine` (see `include/lib.hpp`) stores a `const TSLanguage*` pointer
-- and provides `parse()` and `queryNew()` helpers. Because the `TSLang`
-- handle is owned by Lua wrappers, engines/queries created from them remain
-- valid for the lifetime of those wrappers.
-- - Do NOT store raw `Git` or `File` pointers for use on background threads;
-- `LibGit` uses a `shared_ptr` internally but callbacks invoked from
-- parallel worker threads must not call back into Lua. `findInFiles` returns
-- aggregated results to avoid invoking Lua from worker threads.
-- - `FileReader` data returned by bindings is copied to Lua strings (safe), so
-- you don't get dangling `string_view`s in Lua.

-- Load a language (may clone/compile parser if needed). Keeps parser and engine alive.
local java = loadLanguage("java")

-- Language methods
-- lang:parse(input, isFile) -> Tree
-- if isFile==true, input is a path; otherwise input is source code string
-- lang:queryNew(expr) -> Query -- compile and cache a query for reuse

-- Tree methods
-- tree:sexp() -> string
-- tree:asQuery() -> string
-- tree:query(expr_or_string, cb) -> runs query (string) and calls cb(captures)
-- tree:queryWith(queryObj, cb) -> runs a cached Query object (recommended)
-- tree:matches(expr) -> array of capture-tables (convenience)
--
-- Query objects
-- q = lang:queryNew(expr)
-- q:matches(tree) -> array of capture-tables (convenience)

-- Capture table format (passed to callback):
-- {
-- text = "...",
-- row = 0, col = 0,
-- endRow = 0, endCol = 0,
-- startByte = 0, endByte = 0
-- }

-- File IO
-- r = read(path) -> FileReader
-- r:isValid(), r:sync(), r:get(), r:getRange(from,to), r:getLine(row)
-- r:find(pattern, regex=false) -> list of { startByte, endByte, row, col, text }

-- w = write(path) -> FileWriter
-- w:isValid(), w:save(), w:backup(suffix), w:flush(path)
-- w:append(str), w:insert(offset,str), w:insertRow(row,str), w:deleteRow(row)
-- w:deleteCont(from,to), w:writeAt(offset,str), w:replace(pattern,tpl,nth)
-- w:replaceAll(pattern,tpl)

-- Editor (CST-aware edits)
-- editor = Editor()
-- editor:delete(capture) -- delete node
-- editor:deleteWithPad(capture, pad) -- delete with byte padding
-- editor:insert(capture, text) -- insert at capture start
-- editor:insertAfter(capture, text) -- insert after capture end
-- editor:write(capture, replacement) -- replace node with replacement
-- editor:replace(capture, pattern, tpl) -- regex replace inside capture
-- editor:printBefore(capture), editor:printAfter(capture)
-- errors = editor:apply(tree, writer) -- apply edits; returns table of errors
-- errors = editor:applyAndSave(tree, writer) -- apply edits; if no errors, save automatically
-- Error object returned by apply: { type, startRow, startCol, endRow, endCol, edit={ op, change } }

-- Directory walking
-- walk(path, opts, function(file, git))
-- opts: { ext = {".java"}, recursive = true, filesOnly = true }
-- callback signature receives `File` and `Git` objects. This mirrors C++ behavior.

-- Parallel find helper
-- results = findInFiles(path, pattern, opts)
-- returns an array of { path, row, col, text }

-- Examples

-- Convert createQueryMigrationOldToNew
local java = loadLanguage("java")

local q = [[
 (local_variable_declaration
 type: (generic_type (type_identifier) @q (#eq? @q "Query")
 (type_arguments (type_identifier) @type))
 (variable_declarator (identifier) @var
 (method_invocation (method_invocation (method_invocation
 (identifier) @method_name (#eq? @method_name "createQuery")
 (argument_list [(string_literal) (binary_expression)] @query
 (class_literal (type_identifier) @klass)))))))
 ]]

walk(arg[1], { ext = { ".java" }, recursive = true }, function(file, git)
  local tree = java:parse(file.path, true)
  local w = write(file.path)
  local editor = Editor()

  tree:queryWith(java:queryNew(q), function(c)
    -- named captures: c.type, c.klass
    editor:write(c.type, "") -- delete type
    editor:write(c.klass, "") -- delete klass
  end)

  editor:validate(file)
  local errors = editor:apply(tree, w)
  -- inspect errors if needed
end)

-- Convert staticCastHandling
local java = loadLanguage("java")

local findNativeQuery = [[
 (method_invocation (identifier) @method_name (#eq? @method_name "createNativeQuery")
 arguments: (argument_list [(string_literal) (binary_expression)] @first_arg (class_literal) @second_arg (#eq? @second_arg "Object[].class")))
 ]]

local findCasts = [[
 (cast_expression type: (type_identifier) @cast_type value: (array_access) @cast_value) @cast
 ]]

local wrappers = {
  Character = "StringUtilities.valueOf(",
  String = "StringUtilities.valueOf(",
  Long = "NumberUtilities.toLong(",
  Integer = "NumberUtilities.toInteger(",
}

walk(arg[1], { ext = { ".java" }, recursive = true }, function(file, git)
  local tree = java:parse(file.path, true)
  local w = write(file.path)
  local editor = Editor()

  tree:query(findNativeQuery, function(c)
    if c.second_arg.text ~= "Object[].class" then return end
    tree:query(findCasts, function(cast)
      if cast.cast.row <= c.second_arg.row then return end
      if cast.cast.row > c.second_arg.row + 200 then return end
      local wrapper = wrappers[cast.cast_type.text]
      if not wrapper then return end
      local repl = wrapper .. cast.cast_value.text .. ")"
      editor:write(cast.cast, repl)
    end)
  end)

  editor:apply(tree, w)
end)

-- Simplified workflow (using helpers)
local java = loadLanguage("java")

local q = [[
 (local_variable_declaration
 type: (generic_type (type_identifier) @q (#eq? @q "Query")
 (type_arguments (type_identifier) @type))
 (variable_declarator (identifier) @var
 (method_invocation (method_invocation (method_invocation
 (identifier) @method_name (#eq? @method_name "createQuery")
 (argument_list [(string_literal) (binary_expression)] @query
 (class_literal (type_identifier) @klass)))))))
 ]]

walk(arg[1], { ext = { ".java" }, recursive = true }, function(file, git)
  local tree = java:parse(file.path, true)
  local w = write(file.path)
  local editor = Editor()

  -- use tree:matches to get an array of capture tables
  for _, m in ipairs(tree:matches(q)) do
    editor:write(m.type, "")
    editor:write(m.klass, "")
  end

  -- apply and save if no errors
  local errs = editor:applyAndSave(tree, w)
  -- inspect errs if needed
end)

-- ============================================================
-- Searching a codebase (fast patterns + full queries)
-- ============================================================
-- Using the parallel `findInFiles` helper for simple text/regex searches
local results = findInFiles("src/", "TODO", { recursive = true, ext = { ".java", ".kt" } })
for _, r in ipairs(results) do
  print(r.path, r.row, r.col, r.text)
end

-- Combining `findInFiles` and tree queries: narrow hits, then run tree queries
local hits = findInFiles("src/", "createQuery", { recursive = true, ext = { ".java" } })
for _, h in ipairs(hits) do
  local tree = java:parse(h.path, true)
  -- use a cached query for heavier structural checks
  local qobj = java:queryNew([[ (method_invocation (identifier) @name (#eq? @name "createQuery")) ]])
  for _, m in ipairs(qobj:matches(tree)) do
    -- m is a capture table with .text,.row,.col, etc.
    print("Found createQuery at", h.path, m.row, m.col)
  end
end

-- ============================================================
-- Complex edits and migrations (example pattern)
-- ============================================================
-- Full migration workflow (find, open, parse, perform multiple edits, apply+save)
local migrateQuery = [[
 (method_invocation (identifier) @name (#eq? @name "createQuery"))
 ]]

local cached = java:queryNew(migrateQuery)

walk("project/", { ext = { ".java" }, recursive = true }, function(file, git)
  if not file.isReg then return end
  local r = read(file.path)
  if not r:isValid() then return end
  local src = r:sync().cont
  local tree = java:parse(src, false)
  if not tree then return end

  local w = write(file.path)
  local ed = Editor()

  -- staged approach: collect matches, then perform edits deterministically
  for _, m in ipairs(cached:matches(tree)) do
    -- example: remove deprecated factory call and replace with new API
    ed:replace(m.name, "OldFactory.create(%1)", "NewFactory.build(%1)")
  end

  -- perform a structural edit: delete surrounding type declaration if captured
  local removeTypeQ = [[ (local_variable_declaration type: (type_identifier) @t) ]]
  for _, m in ipairs(tree:matches(removeTypeQ)) do
    ed:deleteWithPad(m.t, 1) -- remove node plus one byte padding
  end

  -- run validator and apply+save atomically
  ed:validate(file)
  local errs = ed:applyAndSave(tree, w)
  if #errs > 0 then
    print("Migration had errors in:", file.path)
  end
end)

-- ============================================================
-- Generating files (create new artifacts from templates)
-- ============================================================
-- Create a new file and write content using `FileWriter` APIs
local outPath = "generated/NewHelper.java"
local fw = write(outPath)
if fw:isValid() then
  fw:append("package generated;\n\n")
  fw:append("public class NewHelper {\n")
  fw:append(" public static void hello() { System.out.println(\"hello\"); }\n")
  fw:append("}\n")
  fw:save()
else
  print("Unable to create", outPath)
end

-- Generate multiple files from matches: create test stubs for classes found
local classQ = [[ (class_declaration name: (type_identifier) @name) ]]
walk("src/", { ext = { ".java" }, recursive = true }, function(file, git)
  local r = read(file.path)
  if not r:isValid() then return end
  local tree = java:parse(r:sync().cont, false)
  if not tree then return end
  for _, m in ipairs(tree:matches(classQ)) do
    local cls = m.name.text:gsub("%s", "")
    local testPath = string.format("tests/%sTest.java", cls)
    local tw = write(testPath)
    if tw:isValid() then
      tw:append(string.format("import %s;\n\npublic class %sTest {\n // TODO: add tests\n}\n", cls, cls))
      tw:save()
    end
  end
end)

-- ============================================================
-- Deep examples: cross-file refactor, repo-wide migration, and reporting
-- These examples combine `findInFiles`, `walk`, `Query`/`Tree` usage,
-- `Editor` edits, `FileWriter` generation, and optional git staging.
-- ============================================================

-- 1) Cross-file class rename (two-pass, safe)
-- Pass A: gather class declarations to build rename map
local renameFrom = "OldService"
local renameTo = "NewService"
local classDeclQ = java:queryNew([[ (class_declaration name: (type_identifier) @name) ]])
local occurrencesQ = java:queryNew([[ (identifier) @id ]])

local candidates = findInFiles("src/", renameFrom, { recursive = true, ext = { ".java" } })
local filesToProcess = {}
for _, c in ipairs(candidates) do filesToProcess[c.path] = true end

-- Pass B: apply rename at identifier sites inside each candidate file
for path, _ in pairs(filesToProcess) do
  local r = read(path)
  if not r:isValid() then goto CONT2 end
  local tree = java:parse(r:sync().cont, false)
  if not tree then goto CONT2 end
  local w = write(path)
  local ed = Editor()
  for _, id in ipairs(occurrencesQ:matches(tree)) do
    if id.id and id.id.text == renameFrom then
      ed:replace(id.id, renameFrom, renameTo)
    end
  end
  ed:validate(File(path))
  local errs = ed:applyAndSave(tree, w)
  if #errs == 0 then print("Renamed in", path) end
  ::CONT2::
end

-- 2) Repo-wide migration with git stage and report
-- Find files with a deprecated pattern, apply structured edits, stage via git
local report = {}
local depPattern = "deprecatedApi"
local depQ = java:queryNew([[ (method_invocation (identifier) @name (#eq? @name "deprecatedApi")) ]])

local matches = findInFiles("./", depPattern, { recursive = true, ext = { ".java" } })
for _, h in ipairs(matches) do
  local r = read(h.path)
  if not r:isValid() then goto CONT3 end
  local src = r:sync().cont
  local tree = java:parse(src, false)
  if not tree then goto CONT3 end
  local w = write(h.path)
  local ed = Editor()
  for _, m in ipairs(depQ:matches(tree)) do
    -- example: wrap first arg in new util and remove deprecated flag
    ed:replace(m.name, "deprecatedApi(%1)", "modernApi.handle(%1)")
  end
  ed:validate(File(h.path))
  local errs = ed:applyAndSave(tree, w)
  table.insert(report, { path = h.path, errors = #errs })
  -- if walk gave us a git object, stage the file
  -- (the findInFiles prefilter doesn't provide git; use walk for git-aware ops)
  ::CONT3::
end

-- Git-aware pass using walk to stage modifications
walk("./", { ext = { ".java" }, recursive = true }, function(file, git)
  if not file.isReg then return end
  -- quick sanity: skip if no changes on disk (this is illustrative)
  if git then
    -- attempt to add file if it was modified by applyAndSave
    pcall(function() git.add(file.path) end)
  end
end)

-- Write a migration report
local repWriter = write("generated/migration_report.txt")
if repWriter:isValid() then
  for i, r in ipairs(report) do
    repWriter:append(string.format("%d: %s errors=%d\n", i, r.path, r.errors))
  end
  repWriter:save()
end

-- 3) Generate helpers based on structure and produce artifacts
-- Example: for each class create a helper and a test stub, using templates
local helperQ = java:queryNew([[ (class_declaration name: (type_identifier) @name) ]])
walk("src/", { ext = { ".java" }, recursive = true }, function(file, git)
  if not file.isReg then return end
  local r = read(file.path)
  if not r:isValid() then return end
  local tree = java:parse(r:sync().cont, false)
  if not tree then return end
  for _, m in ipairs(helperQ:matches(tree)) do
    local cls = m.name.text:gsub("%s", "")
    local helperPath = string.format("generated/%sHelper.java", cls)
    local fw = write(helperPath)
    if fw:isValid() then
      fw:append(string.format("package generated;\n\npublic class %sHelper {\n public static void assist() {}\n}\n", cls))
      fw:save()
    end
    local testPath = string.format("tests/%sHelperTest.java", cls)
    local tw = write(testPath)
    if tw:isValid() then
      tw:append(string.format(
      "package tests;\n\nimport generated.%sHelper;\n\npublic class %sHelperTest {\n // auto-generated test stub\n}\n",
        cls, cls))
      tw:save()
    end
  end
end)

-- 4) Inspect parser output (sexp) and derive a quick query from structure
-- Useful for exploratory migrations: dump S-expression and call `asQuery()`
local sample = read("src/example/Example.java")
if sample and sample:isValid() then
  local tree = java:parse(sample:sync().cont, false)
  if tree then
    print(tree:sexp()) -- human readable tree
    -- derive a textual query by inspection
    local derived = tree:asQuery()
    print("Derived query skeleton:\n", derived)
  end
end

-- ============================================================
-- Exploratory flow: use `sexp()` and `asQuery()` to derive and test queries
-- ============================================================
-- 1) Quick REPL-style exploration for a single file
local samplePath = "src/example/Example.java"
local lang = loadLanguage("java")
local r = read(samplePath)
if r and r:isValid() then
  local src = r:sync().cont
  local tree = lang:parse(src, false)
  if tree then
    -- print the s-expression to inspect structure
    print("S-expression for", samplePath)
    print(tree:sexp())

    -- convert tree shape into a query skeleton
    local skeleton = tree:asQuery()
    print("Query skeleton (edit for specificity):")
    print(skeleton)

    -- as a next step, refine the skeleton into a working query string
    local refined = [[ (method_invocation (identifier) @name) ]]
    local matches = tree:matches(refined)
    print("Matches for refined query:")
    for i, m in ipairs(matches) do
      print(i, m.name and m.name.text, m.name and m.name.row, m.name and m.name.col)
    end
  end
end

-- ============================================================
-- Structure-driven generation: create helper classes and test stubs
-- ============================================================
-- This example scans source files for class declarations and generates
-- a helper and a test stub for each discovered class.
local lang = loadLanguage("java")
local classQuery = [[ (class_declaration name: (type_identifier) @name) ]]

walk("src/", { ext = { ".java" }, recursive = true }, function(file, git)
  if not file.isReg then return end
  local reader = read(file.path)
  if not reader:isValid() then return end
  local src = reader:sync().cont
  local tree = lang:parse(src, false)
  if not tree then return end

  for _, cap in ipairs(tree:matches(classQuery)) do
    if cap.name then
      local cls = cap.name.text:gsub("%s", "")
      -- helper file
      local helperPath = string.format("generated/%sHelper.java", cls)
      local hf = write(helperPath)
      if hf:isValid() then
        hf:append(string.format("package generated;\n\npublic class %sHelper {\n public static void assist() { }\n}\n",
          cls))
        hf:save()
      end

      -- test stub
      local testPath = string.format("tests/%sHelperTest.java", cls)
      local tf = write(testPath)
      if tf:isValid() then
        tf:append(string.format(
        "package tests;\n\nimport generated.%sHelper;\n\npublic class %sHelperTest {\n // auto-generated test stub for %s\n}\n",
          cls, cls, cls))
        tf:save()
      end
    end
  end
end)
