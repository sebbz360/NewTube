r"""
hoist.py - make C99 code acceptable to the XDK's old C89 compiler: move declarations that stand in the MIDDLE of a
block to the START of that block (C89 only allows them there).

  hoist_line(lines, n)   lines = a file's lines (list, edited in place), n = 1-based line of a declaration the
                         compiler rejected ("missing ';' before 'type'" / "illegal use of this type as an expression")

  "size_t n = A->n;"              -> "size_t n;" right after the block's "{"  +  "n = A->n;" where it was
  "const size_t w = 1 << b;"      -> "size_t w;" (+ "w = 1 << b;"): a value assigned later can't stay const
  "const char *begin, *end;"      -> moved as it is (no starting value)
Nothing else changes, so the program does exactly the same.
"""
import re

# a declaration with no starting value, maybe several names: "const char *begin, *end;"
NOINIT = re.compile(r"^\s*(const\s+)?(unsigned\s+|signed\s+|struct\s+)?[A-Za-z_]\w*\s*\**\s*[A-Za-z_]\w*(\s*,\s*\**\s*[A-Za-z_]\w*)*\s*;\s*$")
DECL = re.compile(r"^(\s*)(const\s+)?((?:unsigned\s+|signed\s+|struct\s+)?[A-Za-z_]\w*)(\s*\**\s*)([A-Za-z_]\w*)\s*(=\s*(.*))?$")

def block_start(lines, n):
    """index of the line holding the '{' that opens the block containing line n (1-based)"""
    depth = 0
    for i in range(n - 2, -1, -1):
        text = re.sub(r'"(\\.|[^"\\])*"|\'(\\.|[^\'\\])*\'|//.*$', "", lines[i])   # ignore strings, // comments
        if text.lstrip().startswith("#") or text.lstrip().startswith("*") or text.lstrip().startswith("/*"):
            continue
        for ch in reversed(text):
            if ch == "}":
                depth += 1
            elif ch == "{":
                if depth == 0:
                    return i
                depth -= 1
    return None

FOR = re.compile(r"^(\s*)for\s*\(\s*((?:unsigned\s+|signed\s+)?[A-Za-z_]\w*)(\s*\**\s*)([A-Za-z_]\w*)\s*=")

def hoist_line(lines, n):
    line = lines[n - 1]
    f = FOR.match(line)
    if f:                                       # "for (size_t i = 0; ..."  ->  "size_t i;" up top + "for (i = 0; ..."
        indent, typ, stars, name = f.groups()
        b = block_start(lines, n)
        if b is None:
            return False
        decl = f"{indent}{typ}{stars}{name};   /* (hoist.py: loop variable moved up, for the C89 compiler) */\n"
        lines[n - 1] = line[:f.start(2)] + line[f.end(4):].lstrip()
        lines[n - 1] = re.sub(r"for\s*\(\s*", "for (" + name + " ", lines[n - 1], count=1).replace("for (" + name + " =", "for (" + name + " =", 1)
        # (a loop variable of the same name and type already moved up into this block: don't declare it twice)
        if decl.strip() not in [l.strip() for l in lines[b + 1:n]]:
            lines.insert(b + 1, decl)
        return True
    if NOINIT.match(line) and not re.match(r"^\s*(return|goto)\b", line):   # move the whole line as it is
        b = block_start(lines, n)
        if b is None:
            return False
        del lines[n - 1]
        lines.insert(b + 1, line)
        return True
    m = DECL.match(line.rstrip("\n"))
    if not m:
        return False
    indent, const, typ, stars, name, init, value = m.groups()
    if typ in ("return", "goto", "case", "else", "if", "while", "for", "do", "switch", "sizeof"):
        return False
    b = block_start(lines, n)
    if b is None:
        return False
    if init is None:                                    # no starting value: move the whole line as it is
        moved = line.rstrip("\n")
        del lines[n - 1]
        lines.insert(b + 1, moved.strip() and (indent + moved.strip() + "\n"))
        return True
    # "const size_t w" must lose const (assigned later), but in "const char *p" const means "the TEXT is
    # read-only", not the variable: keep it there
    keep_const = const if (const and "*" in stars) else ""
    decl = f"{indent}{keep_const}{typ}{stars}{name};   /* (hoist.py: moved up from below, for the C89 compiler) */\n"
    lines[n - 1] = f"{indent}{name} = {value}\n"
    lines.insert(b + 1, decl)
    return True
