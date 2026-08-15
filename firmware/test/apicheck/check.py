#!/usr/bin/env python3
"""REST contract check: the two sides of every endpoint must agree.

This started as a route-name comparison, which caught the shape of defect where a
route the UI calls simply does not exist. It could not catch the other shape:

    /api/pins/auto EXISTS, the UI sends `board`, and the handler never reads it —
    so the offline demo produced a correct pin map and the real device handed out
    pins for the wrong chip.

Both compile. Both pass the unit tests. Both pass the browser smoke test, because
the mock backend is what answers there. So this now checks, against
apicheck/contract.tsv:

  * the route exists on both sides (as before);
  * the METHOD the firmware registers matches the contract;
  * every REQUIRED REQUEST field is actually read by the handler;
  * every declared RESPONSE field is actually written by the handler.

What it does not do is execute anything: it is a static agreement check between a
declared contract and two implementations. A handler that reads a field and then
ignores it still passes — that is what the runtime harnesses and the browser test
are for.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
API_JS = os.path.join(REPO, "web-interface", "js", "api.js")
WEB_CPP = os.path.join(REPO, "firmware", "src", "platform", "esp32", "WebApi.cpp")
CONTRACT = os.path.join(HERE, "contract.tsv")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def load_contract():
    rows = []
    for line in read(CONTRACT).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = re.split(r"\s{2,}|\t", line)
        parts = [p.strip() for p in parts if p.strip()]
        if len(parts) != 4:
            sys.exit("contract.tsv: expected 4 columns, got %d: %s" % (len(parts), line))
        path, method, req, resp = parts
        fields = lambda s: [] if s == "-" else [x.strip() for x in s.split(",") if x.strip()]
        # `helper:a,b,c` — the response is written by a named helper rather than
        # inline. Stating it beats teaching the checker to guess: /api/status
        # serves a cached snapshot built by WebApi::fillStatus, deliberately, so
        # that the REST route and the WebSocket broadcast cannot diverge.
        via = None
        if ":" in resp:
            via, resp = resp.split(":", 1)
        rows.append({"path": path, "method": method, "via": via,
                     "request": fields(req), "response": fields(resp)})
    return rows


def ui_routes(js):
    """Route literals the interface calls, path parameters folded to their prefix."""
    out = set()
    for m in re.finditer(r"'(/(?:api|gmb)/[A-Za-z0-9/_.-]*)'", js):
        out.add(m.group(1).split("?")[0])
    return out


def statement_at(src, idx):
    """The whole registering call containing the route literal at `idx`.

    Handlers are registered either as server_->on(path, METHOD, lambda) or by
    constructing an AsyncCallbackJsonWebHandler(path, lambda). In both the literal
    is the FIRST argument, so the call's opening paren is the last '(' before it;
    balancing from there captures exactly one handler body and never bleeds into
    the next one.
    """
    def enclosing_paren(pos):
        """Index of the '(' that OPENS the expression containing `pos`.

        Walking back to the nearest '(' finds a SIBLING call as often as the
        parent, which is how this first returned `(const BoardProfile* b : ...)`
        for a route registered inside a for-loop. Depth tracking finds the parent.
        """
        depth = 0
        for i in range(pos - 1, -1, -1):
            c = src[i]
            if c == ")":
                depth += 1
            elif c == "(":
                if depth == 0:
                    return i
                depth -= 1
        return -1

    def match_from(open_paren):
        depth = 0
        for i in range(open_paren, len(src)):
            c = src[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    return src[open_paren:i + 1]
        return src[open_paren:]

    # Widen until the captured call actually contains a handler. Two shapes need
    # it: a route registered inside a for-loop over the board profiles, and one
    # built from a COMPUTED path — ("/api/board/" + b->identifier).c_str().
    pos = idx
    stmt = src[idx:idx + 200]
    for _ in range(5):
        op = enclosing_paren(pos)
        if op < 0:
            break
        stmt = match_from(op)
        if "[this]" in stmt or "[&]" in stmt or "[=]" in stmt:
            return stmt
        pos = op
    return stmt


def helper_bodies(cpp):
    """File-level helpers a route delegates to, so their fields count as the
    route's own. The status DTO lives in WebApi::fillStatus precisely so the REST
    route and the WebSocket broadcast cannot diverge, and the profile validator is
    a shared lambda — in both cases the fields ARE the route's contract, they are
    just written one call away."""
    out = {}
    for m in re.finditer(r"void WebApi::(\w+)\(", cpp):
        name = m.group(1)
        brace = cpp.find("{", m.end())
        if brace < 0:
            continue
        depth = 0
        for i in range(brace, len(cpp)):
            if cpp[i] == "{":
                depth += 1
            elif cpp[i] == "}":
                depth -= 1
                if depth == 0:
                    out[name] = cpp[brace:i + 1]
                    break
    # `auto name = [capture](params) { body };` — the interesting part is the
    # BODY, so brace-match past the parameter list rather than paren-matching,
    # which would capture only `(params)`.
    for m in re.finditer(r"auto (\w+) = \[[^\]]*\]\([^)]*\)\s*\{", cpp):
        brace = cpp.rfind("{", 0, m.end())
        depth = 0
        for i in range(brace, len(cpp)):
            if cpp[i] == "{":
                depth += 1
            elif cpp[i] == "}":
                depth -= 1
                if depth == 0:
                    out.setdefault(m.group(1), cpp[brace:i + 1])
                    break
    return out


def firmware_handlers(cpp):
    """{(path, method): handler source}. Methods come from the registration."""
    handlers = {}
    for m in re.finditer(r'"(/(?:api|gmb)/[A-Za-z0-9/_.{}-]*)"', cpp):
        path = m.group(1)
        stmt = statement_at(cpp, m.start())
        # server_->on(path, HTTP_GET, ...) states its method inline.
        inline = re.search(r'HTTP_(GET|POST|PUT|DELETE)\s*,', stmt)
        method = inline.group(1) if inline else None
        if method is None:
            # AsyncCallbackJsonWebHandler: the method is set on the object AFTER
            # construction, e.g. `putProfile->setMethod(HTTP_PUT);`. Find the
            # variable the handler was assigned to, then its setMethod call.
            ctor = cpp.rfind("new AsyncCallbackJsonWebHandler", 0, m.start())
            if ctor >= 0:
                decl = re.search(r"auto\*\s+(\w+)\s*=\s*$", cpp[:ctor])
                if decl:
                    sm = re.search(re.escape(decl.group(1)) +
                                   r"->setMethod\(HTTP_(GET|POST|PUT|DELETE)\)", cpp)
                    if sm:
                        method = sm.group(1)
        handlers.setdefault((path, method), "")
        handlers[(path, method)] += stmt
    return handlers


def main():
    js = read(API_JS)
    cpp = read(WEB_CPP)
    contract = load_contract()
    calls = ui_routes(js)
    handlers = firmware_handlers(cpp)
    helpers = helper_bodies(cpp)
    served = {}
    for (path, method), body in handlers.items():
        served.setdefault(path, []).append((method, body))

    problems = []

    # 1. Everything the UI calls must be in the contract, so a new route cannot be
    #    added on one side and quietly skip this check.
    declared = {r["path"] for r in contract}
    for route in sorted(calls):
        if route.endswith("/"):
            if not any(d.startswith(route) or d == route for d in declared):
                problems.append("UI calls %s<param> but contract.tsv declares nothing under it" % route)
        elif route not in declared:
            problems.append("UI calls %s but contract.tsv does not declare it" % route)

    for row in contract:
        path, method = row["path"], row["method"]
        if path.endswith("/"):
            matches = [(p, mb) for p, mbs in served.items() if p.startswith(path) for mb in mbs]
            if not matches:
                problems.append("%s %s<param>: firmware registers nothing under it" % (method, path))
                continue
            bodies = [mb[1] for _, mb in matches]
            methods = {mb[0] for _, mb in matches}
        else:
            if path not in served:
                problems.append("%s %s: firmware does not serve it" % (method, path))
                continue
            bodies = [b for _, b in served[path]]
            methods = {m for m, _ in served[path]}

        if None in methods:
            # An undetermined method must FAIL, not pass. Treating it as "can't
            # tell, so fine" is how a method change on one side slips through — the
            # exact silent degradation this check exists to prevent.
            problems.append("%s %s: cannot determine the method the firmware "
                            "registers (parser needs updating for this shape)"
                            % (method, path))
        elif method not in methods:
            problems.append("%s %s: firmware registers it as %s"
                            % (method, path, "/".join(sorted(str(m) for m in methods))))

        blob = "\n".join(bodies)
        # Pull in any helper the handler delegates to (fillStatus, the shared
        # validator): the fields are still this route's contract.
        for name, body in helpers.items():
            if re.search(r"\b" + re.escape(name) + r"\s*\(", blob):
                blob += "\n" + body
        if row["via"]:
            if row["via"] not in helpers:
                problems.append("%s %s: contract names helper %s, which does not exist"
                                % (method, path, row["via"]))
            else:
                blob += "\n" + helpers[row["via"]]
        for field in row["request"]:
            if field == "@body":
                # The whole body IS the payload (a profile). The check is that the
                # handler actually parses it rather than ignoring it.
                if "fromJson(body" not in blob and "(body," not in blob and "(body)" not in blob:
                    problems.append("%s %s: never parses the request body" % (method, path))
                continue
            # Handlers read the body as body["field"] (ArduinoJson) — the exact
            # spelling the contract can check for.
            if ('body["%s"]' % field) not in blob:
                problems.append('%s %s: never reads request field "%s"' % (method, path, field))
        for field in row["response"]:
            if ('doc["%s"]' % field) not in blob and ('["%s"]' % field) not in blob:
                problems.append('%s %s: never writes response field "%s"' % (method, path, field))

    # A route the firmware serves and nobody calls is a note, not a failure: it may
    # legitimately be a controller endpoint or a debug hook.
    for path in sorted(served):
        if path in calls:
            continue
        if any(c.endswith("/") and path.startswith(c) for c in calls):
            continue
        print("note: firmware serves %s, which the web interface never calls" % path)

    if problems:
        for p in problems:
            print("FAIL: " + p)
        print("\napicheck: %d contract violation(s)" % len(problems))
        return 1
    print("apicheck OK (%d endpoints, methods + request/response fields)" % len(contract))
    return 0


if __name__ == "__main__":
    sys.exit(main())
