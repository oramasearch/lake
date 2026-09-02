#include "lake/api/web_ui.hpp"

namespace lake::api {

std::string web_ui_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string web_ui_stylesheet() {
    return R"css(:root{--surface:#fbfbfa;--ink:#1c1c1a;--muted:#6b6b66;--accent:#ff6b5b;--line:#e6e6e2}
*{box-sizing:border-box}body{margin:0;background:var(--surface);color:var(--ink);
font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
header{position:sticky;top:0;display:flex;align-items:center;gap:.75rem;padding:.75rem 1.25rem;
background:var(--surface);border-bottom:1px solid var(--line)}
header .brand{font-weight:600}header .meta{margin-left:auto;color:var(--muted);
font:13px ui-monospace,SFMono-Regular,Menlo,monospace}
main{max-width:56rem;margin:0 auto;padding:2rem 1.25rem}
.eyebrow{color:var(--accent);font:600 12px/1 ui-monospace,SFMono-Regular,Menlo,monospace;
letter-spacing:.08em;text-transform:uppercase}h1{margin:.4rem 0 .25rem;font-size:2rem}
.sub{color:var(--muted);margin:0 0 1.5rem}.panel{background:#fff;border:1px solid var(--line);
border-radius:12px;padding:1.25rem}code{font:13px ui-monospace,SFMono-Regular,Menlo,monospace}
)css";
}

std::string web_ui_shell(std::string_view title, std::string_view meta, std::string_view content) {
    std::string out;
    out += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
    out += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    out += "<title>";
    out += web_ui_escape(title);
    out += "</title><link rel=\"stylesheet\" href=\"/style.css\"></head><body>";
    out += "<header><span class=\"brand\">lake</span><span class=\"meta\">";
    out += web_ui_escape(meta);
    out += "</span></header><main>";
    out += content;
    out += "</main></body></html>";
    return out;
}

std::string web_ui_hello(std::string_view node_name, std::string_view version) {
    std::string content;
    content += "<p class=\"eyebrow\">hello world</p><h1>hello from lake</h1>";
    content += "<p class=\"sub\">liquid is the tool. lake is where it collects.</p>";
    content += "<div class=\"panel\"><p>This node is <code>";
    content += web_ui_escape(node_name);
    content += "</code>, running lake <code>";
    content += web_ui_escape(version);
    content += "</code>, serving over the same Beast2 + Capy/Corosio stack as Zetta.</p>";
    content += "<p>Machine-readable: <code>GET /v1/server_info</code></p></div>";
    std::string meta = std::string(node_name) + " · " + std::string(version);
    return web_ui_shell("lake — hello", meta, content);
}

}  // namespace lake::api
