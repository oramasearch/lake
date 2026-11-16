// SPDX-License-Identifier: MIT

/** A workspace-prefixed Ask Orama link (`/w/<id>/ask`): Ask Orama's page, `/ask`, with its query
 * (lake#1128). A workspace prefix is remembered by the session first, so the page opens in that
 * workspace. */
import { Navigate, useLocation } from "react-router-dom";
import { askLinkTarget } from "./home";

export default function AskRedirect() {
  const { search } = useLocation();
  return <Navigate to={askLinkTarget(search)} replace />;
}
