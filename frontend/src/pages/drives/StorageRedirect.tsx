/** `/w/<id>/storage` (lake#656): Go to's "Storage", which is the Storage tab of Data → Drives. */
import { Navigate } from "react-router-dom";
import { useWsPath } from "@/pages/train/workspace";

export default function StorageRedirect() {
  const wsPath = useWsPath();
  return <Navigate to={wsPath("/drives?tab=storage")} replace />;
}
