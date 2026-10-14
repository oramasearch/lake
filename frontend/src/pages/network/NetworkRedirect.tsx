/** `/network` opens the section's overview, in the same workspace. */
import { Navigate, useLocation } from "react-router-dom";
import { OVERVIEW_PATH } from "./network";

export default function NetworkRedirect() {
  const { pathname, search } = useLocation();
  return <Navigate to={`${pathname.replace(/\/network\/?$/, OVERVIEW_PATH)}${search}`} replace />;
}
