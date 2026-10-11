/** An old Ask Orama page link (`/ask`, `/w/<id>/ask`): the home, with its query (lake#600). A
 * workspace prefix is remembered by the session first, so the home opens in that workspace. */
import { Navigate, useLocation } from "react-router-dom";
import { askLinkTarget } from "./home";

export default function AskRedirect() {
  const { search } = useLocation();
  return <Navigate to={askLinkTarget(search)} replace />;
}
