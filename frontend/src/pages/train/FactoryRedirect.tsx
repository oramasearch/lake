// SPDX-License-Identifier: MIT

/** The Agent Factory's old address (`/train/agents/new`, moved to Design → Agents in lake#653): the
 * same conversation at `/design/agents/new`, in the same workspace. */
import { Navigate, useLocation } from "react-router-dom";

export default function FactoryRedirect() {
  const { pathname, search } = useLocation();
  return <Navigate to={`${pathname.replace(/\/train\/agents\/new$/, "/design/agents/new")}${search}`} replace />;
}
