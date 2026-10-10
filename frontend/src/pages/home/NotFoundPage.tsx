/** Any address no page answers. The server had no HTML 404 (it answered JSON); this is the error
 * state page in lake's words. */
import { useLocation } from "react-router-dom";
import { useTrail } from "@/lib/trail";
import { Button } from "@/lib/ui";

export default function NotFoundPage() {
  const location = useLocation();
  useTrail([{ label: "Not found" }], "not found");
  return (
    <div className="page-container">
      <div className="state-page error-state" role="alert">
        <p className="eyebrow">not found</p>
        <h1>We Could Not Find This Page</h1>
        <p className="sub">
          Nothing is at <code>{location.pathname}</code>. Check the address, or start again from Home.
        </p>
        <p>
          <Button to="/" variant="primary">
            Home
          </Button>
        </p>
      </div>
    </div>
  );
}
