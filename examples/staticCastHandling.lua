print("staticCastHandling")
Logger.level = 3
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

path = io.read()

local Git = gitOpen(path)
Git:resetHead()
local branch = "test-b-1"
if not Git:branchExists(branch) then Git:branchCreate(branch) end
Git:checkout(branch)

walk(path,
    { ext = {".java"}, ignore = { "**/hibernate/**", "**/datatypes/**", "**/api/**"},
      recursive = true, filesOnly = true },
  function(file, git)
    print(file.path)
    local r = read(file.path);
    if(file.path == "") then return end
    local tree = java:parse(file.path, true)
    local w = write(file.path)
    local edt = Editor()

    tree:query(findNativeQuery,
      function(c)
        if c.second_arg.text ~= "Object[].class" then return end
        tree:query(findCasts,
        function(match)
          if match.cast.row <= c.second_arg.row then return end
          if match.cast.row > c.second_arg.row + 200 then return end
          local wrapper = wrappers[match.cast_type.text]
          if not wrapper then return end
          local repl = wrapper .. match.cast_value.text .. ")"
          edt:write(match.cast, repl)
        end)
      end)

    edt:applyAndSaveValidOnly(tree, w)
    git:add(file.path)
end)
