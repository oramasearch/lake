/** Ask Orama's page (`/ask`, lake#1128): the conversation across the page's width, for a signed-in
 * person; the operator and visitors see the home instead. */
import { Navigate } from "react-router-dom";
import { useSession } from "@/lib/session";
import AskHome from "./AskHome";
import { asksAtHome } from "./home";

export default function AskPage() {
  const { viewer } = useSession();
  if (!asksAtHome(viewer)) return <Navigate to="/" replace />;
  return <AskHome />;
}
