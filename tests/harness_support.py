"""Static guard for the hand-written C stub harnesses.

Every ``test_*_commands.py`` builds a C file by pasting a stub preamble in front
of a slice of production code.  The slice is cut below the ``#include`` block,
so any call the production code makes has to be mirrored by a hand-written stub
or the build only fails at link time.  Checking that statically turns a cryptic

    undefined reference to `burst_led_command'

into a message naming the missing stub.
"""
from pathlib import Path
import re

# Names that never need a definition in the harness.
_IGNORED = {
    # C keywords and operators that look like calls.
    'if', 'for', 'while', 'switch', 'return', 'sizeof', 'defined', 'typeof',
    '__typeof__', '_Static_assert', 'alignof', '_Alignof',
    # libc and compiler builtins.
    'assert', 'snprintf', 'vsnprintf', 'printf', 'fprintf', 'puts', 'putchar',
    'malloc', 'calloc', 'realloc', 'free', 'memcpy', 'memset', 'memcmp',
    'strlen', 'strcmp', 'strncmp', 'strstr', 'strchr', 'strcpy', 'strncpy',
    'sscanf', 'abort', 'exit',
    # Compiler and stdarg forms that are spelled like calls.
    '__attribute__', '__builtin_offsetof', 'offsetof', 'va_start', 'va_arg',
    'va_end', 'va_copy',
}

_CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
_DEFINE = re.compile(r'(?m)^\s*#define\s+([A-Za-z_]\w*)\s*\(')
_IDENT = re.compile(r'\b[A-Za-z_]\w*\b')
_INCLUDE = re.compile(r'(?m)^\s*#include\s+"([^"]+)"')
_COMMENT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
_OBJECT_DEFINE = re.compile(r'^\s*#\s*define\s+([A-Za-z_]\w*)\s+(\S+)\s*$')
_UNDEF = re.compile(r'^\s*#\s*undef\s+([A-Za-z_]\w*)\s*$')


def _strip_comments(text):
    return _COMMENT.sub(' ', text)


def _definitions(text):
    """Names that appear in a definition or a function-like macro."""
    names = set(_DEFINE.findall(text))
    for m in _CALL.finditer(text):
        i, depth = m.end() - 1, 0
        while i < len(text):
            if text[i] == '(':
                depth += 1
            elif text[i] == ')':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        else:
            continue
        j = i + 1
        while j < len(text) and text[j] in ' \t\r\n':
            j += 1
        if j < len(text) and text[j] == '{':
            names.add(m.group(1))
    return names


def _quoted_headers(text, include_dirs, seen):
    """Contents of every ``#include "..."`` reachable from ``text``."""
    found = []
    for name in _INCLUDE.findall(text):
        for base in include_dirs:
            path = Path(base)/name
            if path in seen or not path.is_file():
                continue
            seen.add(path)
            body = path.read_text()
            found.append(body)
            found.extend(_quoted_headers(body, [path.parent, *include_dirs], seen))
            break
    return found


def _macro_env(texts, flags):
    """Rough macro table: -D flags plus every object-like ``#define``."""
    env = {}
    for flag in flags:
        if flag.startswith('-D'):
            name, _, value = flag[2:].partition('=')
            env[name] = int(value) if value.lstrip('-').isdigit() else 1
        elif flag.startswith('-U'):
            env.pop(flag[2:], None)
    for text in texts:
        for line in text.splitlines():
            m = _OBJECT_DEFINE.match(line)
            if m:
                value = m.group(2)
                env[m.group(1)] = int(value) if value.lstrip('-').isdigit() else 1
                continue
            m = _UNDEF.match(line)
            if m:
                env.pop(m.group(1), None)
    return env


def _eval(expr, env):
    expr = re.sub(r'defined\s*\(\s*(\w+)\s*\)',
                  lambda m: '1' if m.group(1) in env else '0', expr)
    expr = re.sub(r'defined\s+(\w+)',
                  lambda m: '1' if m.group(1) in env else '0', expr)
    expr = _IDENT.sub(lambda m: str(env.get(m.group(0), 0)), expr)
    expr = expr.replace('&&', ' and ').replace('||', ' or ')
    expr = re.sub(r'!(?!=)', ' not ', expr)
    try:
        return bool(eval(expr, {'__builtins__': {}}, {}))
    except Exception:
        return True  # Unparseable condition: assume the code is live.


def _live_lines(text, env):
    """Keep only the lines the preprocessor would compile."""
    out, stack, live = [], [], True
    for line in text.splitlines():
        stripped = line.lstrip()
        if not stripped.startswith('#'):
            if live:
                out.append(line)
            continue
        body = stripped[1:].strip()
        word, _, rest = body.partition(' ')
        if word in ('if', 'ifdef', 'ifndef'):
            if word == 'if':
                taken = _eval(rest, env)
            elif word == 'ifdef':
                taken = rest.strip() in env
            else:
                taken = rest.strip() not in env
            stack.append([live, taken])
            live = live and taken
        elif word == 'elif' and stack:
            parent, taken = stack[-1]
            branch = parent and not taken and _eval(rest, env)
            stack[-1][1] = taken or branch
            live = branch
        elif word == 'else' and stack:
            parent, taken = stack[-1]
            live = parent and not taken
            stack[-1][1] = True
        elif word == 'endif' and stack:
            live = stack.pop()[0]
        elif live:
            out.append(line)
    return '\n'.join(out)


def missing_stubs(*sources, include_dirs=(), flags=()):
    """Function names the sources call but never define, stub or include.

    Pass every translation unit that ends up in the same binary: a symbol
    defined in one of them satisfies a call from another, exactly like the
    linker would see it.  ``flags`` are the compiler arguments, so ``-D``
    options and preprocessor conditionals agree with the real build.
    """
    stripped = [_strip_comments(s) for s in sources]
    dirs = [Path(d) for d in include_dirs]
    dirs += [Path(f[2:]) for f in flags
             if isinstance(f, str) and f.startswith('-I')]
    headers = '\n'.join(_quoted_headers('\n'.join(stripped), dirs, set()))
    headers = _strip_comments(headers)
    env = _macro_env([*stripped, headers], flags)
    body = _live_lines('\n'.join(stripped), env)
    headers = _live_lines(headers, env)
    defined = _definitions(body) | _definitions(headers)
    return sorted({n for n in _CALL.findall(body)
                   if n not in defined | _IGNORED and not n.isupper()})


def assert_stubs(*sources, include_dirs=(), flags=(), label='harness'):
    """Fail with the missing stub names instead of letting the linker complain."""
    missing = missing_stubs(*sources, include_dirs=include_dirs, flags=flags)
    if missing:
        raise AssertionError(
            f'{label}: {len(missing)} call(s) have no stub or definition: '
            + ', '.join(missing)
        )
