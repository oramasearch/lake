/** A render error stays inside the screen that hit it: the shell, the navigation and every other
 * screen keep working, and the person sees what happened instead of a blank page. Resets when the
 * route changes. */
import { Component, type ReactNode } from "react";
import { useLocation } from "react-router-dom";
import { Button, Notice } from "@/lib/ui";

type State = { error: Error | null };

class Boundary extends Component<{ children: ReactNode; resetKey: string }, State> {
  state: State = { error: null };

  static getDerivedStateFromError(error: Error): State {
    return { error };
  }

  componentDidCatch(error: Error) {
    console.error("lake screen error:", error);
  }

  componentDidUpdate(prev: { resetKey: string }) {
    if (prev.resetKey !== this.props.resetKey && this.state.error) this.setState({ error: null });
  }

  render() {
    if (!this.state.error) return this.props.children;
    return (
      <div className="page-container">
        <Notice tone="danger" title="This page hit an error">
          The rest of lake still works: choose another page, or reload this one. The details are in the browser console.
          <div className="mt-2">
            <Button onClick={() => window.location.reload()}>Reload</Button>
          </div>
        </Notice>
      </div>
    );
  }
}

export function ScreenBoundary({ children }: { children: ReactNode }) {
  const location = useLocation();
  return <Boundary resetKey={location.pathname}>{children}</Boundary>;
}
