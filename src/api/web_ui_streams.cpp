#include "lake/api/web_ui.hpp"

#include "lake/api/oauth.hpp"

namespace lake::api {
namespace {
using nlohmann::json;

std::string hidden(std::string_view name, std::string_view value) {
    return "<input type=\"hidden\" name=\"" + web_ui_escape(name) + "\" value=\"" +
           web_ui_escape(value) + "\">";
}
std::string scope(const StreamsPage& page) {
    return hidden("customer_id", page.customer_id) + hidden("zetta", page.zetta) +
           hidden("topic", page.topic);
}
std::string form(std::string_view capability) {
    return "<form method=\"post\" data-capability=\"" + std::string(capability) + "\">";
}
std::string pretty(std::string payload) {
    const auto parsed = json::parse(payload, nullptr, false);
    return parsed.is_discarded() ? payload : parsed.dump(2);
}

std::string message(const StreamsPage& page, const json& event, bool writable) {
    const auto sequence = event.at("sequence").dump();
    const auto revision = event.at("revision").dump();
    const auto payload = pretty(event.at("payload"));
    std::string html = "<article class=\"panel\" data-sequence=\"" + sequence + "\"><h3>Message " +
        sequence + "</h3><p data-message-meta>Sender: " + web_ui_escape(event.at("from").get<std::string>()) +
        " · Published: " + event.at("published_at").dump() + " (Unix ms) · Size: " +
        event.at("size").dump() + " bytes · Revision: " + revision + "</p>";
    html += "<details data-payload-details" + std::string(payload.size() <= 1024 ? " open" : "") +
        "><summary>JSON Payload</summary><pre data-payload>" + web_ui_escape(payload) + "</pre></details>";
    html += "<button type=\"button\" data-copy-message data-capability=\"browse_stream\">Copy message</button>";
    if (writable && page.can_write) {
        html += "<details><summary>Edit Payload</summary>" + form("update_stream_message") + scope(page) +
            hidden("sequence", sequence) + hidden("expected_revision", revision) +
            "<label>JSON payload<textarea name=\"payload\" required>" + web_ui_escape(payload) +
            "</textarea></label><label><input type=\"checkbox\" name=\"redeliver\"> "
            "Re-deliver to subscribers that already received it</label><p>Off by default. "
            "Enabling this queues the edited payload again for subscriptions that received this "
            "message, including those that acknowledged it. Pending subscribers receive the edit "
            "either way.</p><button type=\"submit\">Save payload</button></form></details>";
    }
    if (writable && page.can_admin) {
        html += form("delete_stream_messages") + scope(page) + hidden("sequences", "[" + sequence + "]") +
            "<button class=\"danger\" type=\"submit\">Delete message</button></form>";
    }
    return html + "</article>";
}

constexpr std::string_view script = R"html(<script id="streams-controls">
(() => {
  const root = document.getElementById('streams');
  const notice = document.getElementById('stream-notice');
  const showError = error => {
    notice.textContent = error.message;
  };
  async function call(name, input, signal) {
    const response = await fetch('/v1/' + name, {
      method: 'POST', credentials: 'same-origin', signal,
      headers: {'Content-Type': 'application/json'}, body: JSON.stringify(input)
    });
    const result = await response.json();
    if (!result.ok) throw new Error(result.error.message);
    return result.value;
  }
  function inputOf(form) {
    const input = Object.fromEntries(new FormData(form));
    for (const key of ['sequence', 'expected_revision', 'before_sequence']) {
      if (key in input) input[key] = Number(input[key]);
    }
    for (const key of ['settings', 'sequences']) {
      if (key in input) input[key] = JSON.parse(input[key]);
    }
    if ('payload' in input) JSON.parse(input.payload);
    if (form.elements.redeliver) input.redeliver = form.elements.redeliver.checked;
    return input;
  }
  root.addEventListener('submit', async event => {
    const form = event.target;
    const name = form.dataset.capability;
    if (!name || form.method === 'get') return;
    event.preventDefault();
    const button = form.querySelector('button[type="submit"]');
    button.disabled = true;
    try {
      const input = inputOf(form);
      if (name === 'update_stream') {
        const preview = await call(name, {...input, dry_run: true});
        if (!confirm('Apply these settings? This currently trims ' + preview.trimmed +
                     ' held messages. Messages published or expired meanwhile may change the count.')) return;
      } else if (name === 'delete_stream_messages') {
        const preview = await call(name, {...input, dry_run: true});
        if (!confirm('Delete ' + preview.removed +
                     ' held messages? Subscribers that have not received them will skip them.')) return;
        input.confirm_count = preview.removed;
      } else if (name === 'delete_stream') {
        if (input.confirm_name !== input.topic && 'topic:' + input.confirm_name !== input.topic) {
          throw new Error('Type the exact topic name to confirm deletion.');
        }
      }
      const result = await call(name, input);
      if (name === 'publish_stream_message' && !result.accepted) {
        throw new Error('Zetta did not accept the message. Check the topic limits and overflow policy.');
      }
      if (name === 'create_stream') {
        location.href = '/train/stream?' + new URLSearchParams({customer_id: input.customer_id,
                          zetta: 'workspace', topic: result.topic});
      } else if (name === 'delete_stream') {
        location.href = '/train/streams?' + new URLSearchParams({customer_id: input.customer_id});
      } else {
        location.reload();
      }
    } catch (error) {
      showError(error);
    } finally {
      button.disabled = false;
    }
  });
  root.addEventListener('click', async event => {
    const button = event.target.closest('[data-copy-message]');
    if (!button) return;
    try {
      await navigator.clipboard.writeText(button.closest('article').querySelector('[data-payload]').textContent);
      button.textContent = 'Copied';
    } catch (error) {
      showError(error);
    }
  });
  const live = document.getElementById('stream-live');
  if (!live) return;
  const list = document.getElementById('stream-messages');
  let cursor = Number(root.dataset.nextSequence);
  let controller;
  function insert(event) {
    if (list.querySelector('[data-sequence="' + event.sequence + '"]')) return;
    const node = document.getElementById('stream-message-template').content.firstElementChild.cloneNode(true);
    node.dataset.sequence = event.sequence;
    node.querySelector('h3').textContent = 'Message ' + event.sequence;
    node.querySelector('[data-message-meta]').textContent = 'Sender: ' + event.from +
      ' · Published: ' + event.published_at + ' (Unix ms) · Size: ' + event.size + ' bytes · Revision: ' + event.revision;
    let payload = event.payload;
    try {
      payload = JSON.stringify(JSON.parse(payload), null, 2);
    } catch {
      // Non-JSON system events remain readable as text.
    }
    node.querySelector('[data-payload]').textContent = payload;
    node.querySelector('[data-payload-details]').open = payload.length <= 1024;
    for (const form of node.querySelectorAll('form')) {
      if (form.elements.payload) form.elements.payload.value = payload;
      if (form.elements.sequence) form.elements.sequence.value = event.sequence;
      if (form.elements.expected_revision) form.elements.expected_revision.value = event.revision;
      if (form.elements.sequences) form.elements.sequences.value = JSON.stringify([event.sequence]);
    }
    list.prepend(node);
    while (list.children.length > 200) list.lastElementChild.remove();
  }
  live.addEventListener('change', async () => {
    if (controller) controller.abort();
    if (!live.checked) return;
    controller = new AbortController();
    const signal = controller.signal;
    try {
      while (!signal.aborted) {
        const page = await call('browse_stream', {customer_id: root.dataset.customer,
          zetta: root.dataset.zetta, topic: root.dataset.topic, from_sequence: cursor,
          direction: 'forward', limit: 100, wait_ms: 10000}, signal);
        if (signal.aborted) break;
        for (const event of page.events) insert(event);
        cursor = page.next_page_sequence ?? page.next_sequence;
        await new Promise(resolve => setTimeout(resolve, 250));
      }
    } catch (error) {
      if (error.name !== 'AbortError') {
        showError(error);
        live.checked = false;
      }
    }
  });
})();
</script>)html";
} // namespace

std::string web_ui_streams(const StreamsPage& page, WebUiViewer viewer) {
    const auto escape = web_ui_escape;
    const std::string query = "customer_id=" + oauth_url_encode(page.customer_id);
    const std::string base = "/train/stream?" + query + "&zetta=" + oauth_url_encode(page.zetta) +
                             "&topic=" + oauth_url_encode(page.topic);
    std::string html = "<section id=\"streams\" data-customer=\"" + escape(page.customer_id) +
        "\" data-zetta=\"" + escape(page.zetta) + "\" data-topic=\"" + escape(page.topic) +
        "\" data-next-sequence=\"" + page.messages.value("next_sequence", json(1)).dump() + "\">";
    html += "<p class=\"eyebrow\">Train</p><h1>Streams</h1><p>Browse retained messages without "
            "consuming them. Live reads leave subscriber positions unchanged.</p>";
    html += "<p id=\"stream-notice\" role=\"status\">" + escape(page.error) + "</p>";
    html += "<noscript><p>Reading and paging work without JavaScript. Enable JavaScript for live "
            "updates and confirmed changes, or use the same stream capabilities through REST or MCP.</p></noscript>";
    if (page.topic.empty()) {
        html += "<form method=\"get\" action=\"/train/streams\" data-capability=\"list_streams\">"
            "<label>Space<select name=\"customer_id\">";
        for (const auto& customer : page.customers) {
            html += "<option value=\"" + escape(customer.id) + "\"" +
                (customer.id == page.customer_id ? " selected" : "") + ">" + escape(customer.handle) + "</option>";
        }
        html += "</select></label><label>Name prefix<input name=\"prefix\" value=\"" + escape(page.prefix) +
            "\"></label><button type=\"submit\">Filter</button></form>";
        for (const auto& group : page.listing.value("groups", json::array())) {
            const std::string zetta = group.at("zetta");
            html += "<section class=\"panel\"><h2>" + std::string(zetta == "bus" ? "Lake Bus" : "Workspace Zetta") +
                "</h2><p>" + escape(group.value("error", "")) + "</p><table><thead><tr><th>Topic</th>"
                "<th>Subscribers</th><th>Held Events</th><th>Next Sequence</th></tr></thead><tbody>";
            for (const auto& topic : group.at("topics")) {
                const std::string name = topic.at("topic");
                html += "<tr><td><a data-capability=\"get_stream\" href=\"/train/stream?" + query +
                    "&amp;zetta=" + zetta + "&amp;topic=" + oauth_url_encode(name) + "\">" + escape(name) +
                    "</a>" + (topic.value("system", false) ? " <span class=\"badge\">system</span>" : "") +
                    "</td><td>" + topic.at("subscribers").dump() + "</td><td>" + topic.at("held_events").dump() +
                    "</td><td>" + topic.at("next_sequence").dump() + "</td></tr>";
            }
            html += "</tbody></table></section>";
        }
        if (page.can_write) {
            html += "<section class=\"panel\"><h2>New Topic</h2>" + form("create_stream") +
                hidden("customer_id", page.customer_id) + hidden("zetta", "workspace") +
                "<label>Topic name<input name=\"topic\" required></label>"
                "<label>Settings (JSON)<textarea name=\"settings\" rows=\"10\" required>"
                "{\n  &quot;retention_ms&quot;: 86400000,\n  &quot;max_events&quot;: 1000,\n"
                "  &quot;delivery&quot;: {&quot;guarantee&quot;: &quot;at_least_once&quot;}\n}"
                "</textarea></label><p>Configure retention_ms, max_events, memory_cap_bytes, "
                "disk_cap_bytes and delivery defaults. Null clears an optional override. "
                "New topics live on this space's workspace Zetta.</p><button type=\"submit\">"
                "Create topic</button></form></section>";
        }
    } else if (!page.detail.empty()) {
        const bool system = page.detail.value("system", true);
        const bool manage = page.detail.value("can_manage", false);
        html += "<p><a href=\"/train/streams?" + query + "\">All streams</a></p><h2>" + escape(page.topic) +
            "</h2>" + (system ? "<p class=\"badge\">system · browse-only</p>" : "") +
            "<details><summary>Settings</summary><pre>" + escape(page.detail.at("settings").dump(2)) + "</pre></details>";
        html += "<h2>Subscriptions</h2><table><thead><tr><th>Subscriber</th><th>Position</th>"
            "<th>Next Sequence</th><th>Lag</th></tr></thead><tbody>";
        for (const auto& subscription : page.detail.at("subscriptions")) {
            html += "<tr><td>" + escape(subscription.at("subscriber").get<std::string>()) + "</td><td>" +
                escape(subscription.at("positions").dump()) + "</td><td>" + subscription.at("next_sequence").dump() +
                "</td><td>" + subscription.at("lag").dump() + "</td></tr>";
        }
        html += "</tbody></table>";
        if (!system && manage && page.can_write) {
            json mutable_settings = page.detail.at("settings");
            for (const auto* field : {"lifecycle", "store", "ring_capacity", "inline_threshold",
                                      "priority_levels", "producer_queue", "exposed"}) {
                mutable_settings.erase(field);
            }
            html += "<details><summary>Edit Settings</summary>" + form("update_stream") + scope(page) +
                "<label>Mutable settings (JSON)<textarea name=\"settings\" rows=\"12\" required>" +
                escape(mutable_settings.dump(2)) + "</textarea></label><p>A retention shrink removes "
                "old messages immediately. The confirmation shows the current trim count.</p>"
                "<button type=\"submit\">Preview and save settings</button></form></details>";
        }
        html += "<h2>Messages</h2><form method=\"get\" action=\"/train/stream\" data-capability=\"browse_stream\">" +
            scope(page) + "<label>Jump to sequence<input type=\"number\" min=\"1\" name=\"from_sequence\" required>"
            "</label><button type=\"submit\">Jump</button></form>"
            "<label><input id=\"stream-live\" type=\"checkbox\" data-capability=\"browse_stream\"> Live</label>"
            "<p>Uncheck Live to pause. Copy and edit use the displayed revision. Refresh to see edits "
            "to older messages.</p><div id=\"stream-messages\">";
        for (const auto& event : page.messages.value("events", json::array())) {
            html += message(page, event, !system);
        }
        html += "</div><template id=\"stream-message-template\">" + message(page,
            {{"sequence", 1}, {"revision", 1}, {"payload", "{}"}, {"from", ""}, {"published_at", 0}, {"size", 2}},
            !system) + "</template>";
        if (!page.messages.value("next_page_sequence", json()).is_null()) {
            html += "<a data-capability=\"browse_stream\" href=\"" + escape(base) + "&amp;from_sequence=" +
                page.messages.at("next_page_sequence").dump() + "\">Older messages</a>";
        }
        html += "<p><a data-capability=\"browse_stream\" href=\"" + escape(base) + "\">Newest messages</a></p>";
        if (!system && page.can_write) {
            html += "<h2>Publish Message</h2>" + form("publish_stream_message") + scope(page) +
                "<label>JSON payload<textarea name=\"payload\" required>{}</textarea></label>"
                "<button type=\"submit\">Publish</button></form>";
        }
        if (!system && page.can_admin) {
            html += "<h2>Purge Messages</h2>" + form("delete_stream_messages") + scope(page) +
                "<label>Before sequence (exclusive)<input name=\"before_sequence\" type=\"number\" min=\"1\" required>"
                "</label><p>Subscribers that have not received these messages will skip them.</p>"
                "<button type=\"submit\">Preview purge</button></form>";
            if (manage) {
                html += "<h2>Delete Topic</h2>" + form("delete_stream") + scope(page) +
                    "<label>Type the topic name<input name=\"confirm_name\" required></label>"
                    "<p>This removes all held messages and closes subscriptions.</p>"
                    "<button class=\"danger\" type=\"submit\">Delete topic</button></form>";
            }
        }
    }
    html += std::string(script) + "</section>";
    return web_ui_shell("lake — Streams", {{"Train", "/train/runs", ""},
        {"Streams", "/train/streams?" + query, ""}}, html, viewer);
}
} // namespace lake::api
