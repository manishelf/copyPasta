
path = io.read()

local java = loadLanguage("java")
print("find createQuery instances")
local hits = findInFiles(path, [[createQuery\(\s*.*,\s*Object\[\]\.class\s*\)]], { ext = { ".java" } })

local i = 1;
for _, h in ipairs(hits) do
  for _, m in ipairs(h) do
    local pos = m
    print(pos.path .. ":" .. pos.row .. ":" .. pos.col)
    print(pos.text)
    print(i);
    i = i+1;
  end
end


