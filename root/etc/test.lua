math.randomseed(os.time())

local function add(a, b) return a + b end
local function subtract(a, b) return a - b end
local function multiply(a, b) return a * b end
local function divide(a, b) if b == 0 then return nil end return a / b end

local function greet(name)
  print("Hello, " .. (name or "friend") .. "!")
end

local function factorial(n)
  local result = 1
  for i = 2, n do result = result * i end
  return result
end

local function read_line(prompt)
  if prompt then io.write(prompt) end
  local s = io.read()
  if s == nil then os.exit() end
  return s
end

local function read_number(prompt, opts)
  opts = opts or {}
  while true do
    local s = read_line(prompt)
    local n = tonumber(s)
    if not n then
      print("Invalid number. Try again.")
    elseif opts.integer and math.floor(n) ~= n then
      print("Please enter an integer.")
    elseif opts.nonnegative and n < 0 then
      print("Please enter a non-negative number.")
    elseif opts.nozero and n == 0 then
      print("Zero is not allowed. Try again.")
    else
      return n
    end
  end
end

local function calculator()
  while true do
    print("\nCalculator:")
    print(" 1) Add")
    print(" 2) Subtract")
    print(" 3) Multiply")
    print(" 4) Divide")
    print(" 5) Back to main menu")
    local choice = read_line("Choose (1-5): ")
    if choice == "5" then return end
    if choice == "1" or choice == "2" or choice == "3" or choice == "4" then
      local a = read_number("Enter first number: ")
      local b = read_number("Enter second number: ")
      if choice == "1" then print("Result: " .. add(a, b))
      elseif choice == "2" then print("Result: " .. subtract(a, b))
      elseif choice == "3" then print("Result: " .. multiply(a, b))
      else
        if b == 0 then print("Cannot divide by zero.") else print("Result: " .. divide(a, b)) end
      end
    else
      print("Invalid choice.")
    end
  end
end

local function guessing_game()
  local target = math.random(1, 100)
  local tries = 0
  print("I'm thinking of a number between 1 and 100. Try to guess it!")
  while true do
    tries = tries + 1
    local guess = read_number("Your guess: ", {integer=true, nonnegative=true})
    if guess < target then
      print("Too low!")
    elseif guess > target then
      print("Too high!")
    else
      print("Correct! You got it in " .. tries .. " tries.")
      break
    end
  end
end

local function view_file()
  local path = read_line("Enter file path to view: ")
  local f, err = io.open(path, "r")
  if not f then print("Cannot open file: " .. (err or "unknown")) return end
  for line in f:lines() do print(line) end
  f:close()
end

local function list_dir()
  local p = io.popen("ls -la")
  if not p then print("ls not available") return end
  for line in p:lines() do print(line) end
  p:close()
end

local function reverse_string()
  local s = read_line("Enter text to reverse: ")
  local rev = s:reverse()
  print("Reversed: " .. rev)
end

while true do
  print("\nWelcome to the interactive Lua script!")
  print("Choose an action:")
  print(" 1. Calculator")
  print(" 2. Greet a name")
  print(" 3. Compute factorial")
  print(" 4. Number guessing game")
  print(" 5. View a file")
  print(" 6. List current directory")
  print(" 7. Reverse text")
  print(" 8. Quit")

  local choice = read_line("Enter your choice (1-8): ")
  if choice == "1" then
    calculator()
  elseif choice == "2" then
    local name = read_line("Enter a name: ")
    greet(name)
  elseif choice == "3" then
    local n = read_number("Enter a non-negative integer: ", {integer=true, nonnegative=true})
    print("Factorial: " .. factorial(n))
  elseif choice == "4" then
    guessing_game()
  elseif choice == "5" then
    view_file()
  elseif choice == "6" then
    list_dir()
  elseif choice == "7" then
    reverse_string()
  elseif choice == "8" then
    print("Goodbye!")
    break
  else
    print("Invalid choice. Please select 1-8.")
  end
end
