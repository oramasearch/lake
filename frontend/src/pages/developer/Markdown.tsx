/** Markdown rendered in the browser (react-markdown + GFM), never as raw HTML. Links to app pages
 * (`/docs/...`) stay in the app; every other link is a plain link, so server routes such as
 * `/llms.txt` and `/v1/openapi.json` load from the server. Unsafe schemes are dropped by
 * react-markdown's URL filter. */
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import { Link } from "react-router-dom";

/** App routes a Markdown link may open without leaving the app. */
const APP_LINK = /^\/docs(\/|$)/;

export function Markdown({ children }: { children: string }) {
  return (
    <div className="markdown-body">
      <ReactMarkdown
        remarkPlugins={[remarkGfm]}
        components={{
          a: ({ href, children: text }) =>
            href && APP_LINK.test(href) ? <Link to={href}>{text}</Link> : <a href={href}>{text}</a>,
        }}
      >
        {children}
      </ReactMarkdown>
    </div>
  );
}
