// SPDX-License-Identifier: MIT

/** "From your phone": a short help, opened in place, for bringing a voice memo, a recorded call or a
 * file from an iPhone or an Android phone into lake (lake#561), ported from rev-os's
 * `components/phone/FromYourPhone.tsx`. Beside the microphone in Ask Orama and beside Upload on the
 * Transcripts page. It opens for the phone the person is using (from the browser), with the other
 * one a tap away. Details that change between models and versions are left to Apple's and Google's
 * support pages, linked rather than copied. */
import { useState, type ReactNode } from "react";
import { ArrowUpRight, Smartphone, X } from "lucide-react";
import { Link } from "react-router-dom";
import { cx } from "@/lib/ui";
import { ACCEPTED_RECORDINGS, ACCEPTED_TEXT, phoneKind, type PhoneKind } from "@/lib/phone";

/** The device in use, read once; on a computer, iPhone first. */
function detected(): PhoneKind {
  if (typeof navigator === "undefined") return "iphone";
  return phoneKind(navigator.userAgent, navigator.maxTouchPoints ?? 0) ?? "iphone";
}

/** The link that opens the help. */
export function FromYourPhoneLink({ open, onToggle, className }: { open: boolean; onToggle: () => void; className?: string }) {
  return (
    <button
      type="button"
      onClick={onToggle}
      aria-expanded={open}
      className={cx("plain hit inline-flex min-h-7 items-center gap-1 rounded text-[12px] text-ink-3 hover:text-ink", className)}
    >
      <Smartphone size={12} aria-hidden />
      From your phone
    </button>
  );
}

function Ext({ href, children }: { href: string; children: ReactNode }) {
  return (
    <a href={href} target="_blank" rel="noreferrer" className="inline-flex items-center gap-0.5">
      {children}
      <ArrowUpRight size={11} aria-hidden />
    </a>
  );
}

/** The help itself. `where` names how the file reaches lake from here: Upload on the Transcripts
 * page, or (from Ask Orama) the Transcripts page itself. On a phone its tabs and Close are 44 px,
 * like the sheet it opens in. */
export function FromYourPhonePanel({ where, transcriptsPath, onClose, className }: { where: "upload" | "ask"; transcriptsPath?: string; onClose: () => void; className?: string }) {
  const [phone, setPhone] = useState<PhoneKind>(detected);
  const then =
    where === "upload" ? (
      <>
        here, tap <b>Upload audio</b>
      </>
    ) : (
      <>
        open {transcriptsPath ? <Link to={transcriptsPath}>Transcripts</Link> : "Transcripts"}, tap <b>New transcript</b>, then <b>Upload audio</b>; ask about it from its page
      </>
    );
  const tab = (id: PhoneKind, label: string) => (
    <button
      type="button"
      role="tab"
      aria-selected={phone === id}
      onClick={() => setPhone(id)}
      className={cx("plain rounded-[5px] px-2.5 py-1 text-[12.5px] max-md:min-h-[44px] max-md:px-3.5", phone === id ? "bg-surface font-semibold text-ink shadow-popover" : "text-ink-3 hover:text-ink")}
    >
      {label}
    </button>
  );
  return (
    <section aria-label="From your phone" className={cx("rounded-[6px] border border-hairline bg-canvas p-3 text-[12.5px] leading-[18px] text-ink-2", className)}>
      <div className="mb-2 flex items-center gap-2">
        <div role="tablist" aria-label="Your phone" className="inline-flex gap-0.5 rounded-[6px] bg-raised p-0.5">
          {tab("iphone", "iPhone")}
          {tab("android", "Android")}
        </div>
        <span className="flex-1" />
        <button type="button" onClick={onClose} aria-label="Close" className="plain inline-flex h-[44px] w-[44px] items-center justify-center rounded text-ink-3 hover:bg-raised hover:text-ink md:h-6 md:w-6">
          <X size={13} />
        </button>
      </div>
      {phone === "iphone" ? (
        <ol className="m-0 list-decimal space-y-1 pl-4">
          <li>
            <b>A voice memo:</b> in Voice Memos, open the recording, tap <b>•••</b> or Share, then <b>Save to Files</b>: On My iPhone or iCloud Drive, in Downloads.
          </li>
          <li>
            <b>A call you recorded:</b> in Notes, open <b>Call Recordings</b>, then the call's note, tap <b>•••</b>, and save or share the audio to <b>Files</b>.
          </li>
          <li>
            <b>Then:</b> {then}, choose <b>Choose File</b> (or Browse), open the folder and pick the <span className="font-mono">.m4a</span>.
          </li>
        </ol>
      ) : (
        <ol className="m-0 list-decimal space-y-1 pl-4">
          <li>
            <b>A voice recording:</b> in Recorder, Voice Recorder (Samsung) or Sound Recorder, open the recording, tap <b>Share</b> (or ⋮) and save it to Files or My Files: Internal storage, in <b>Download</b>.
          </li>
          <li>
            <b>A call you recorded:</b> in the Phone app, open Recents, the contact, <b>History</b>, then the call, and tap <b>Share</b> to Files or Drive. A file manager can't see it until you share it.
          </li>
          <li>
            <b>Then:</b> {then}, open Files or My Files, then ☰ → <b>Downloads</b>, Audio or Documents, and pick the recording.
          </li>
        </ol>
      )}
      <p className="mt-2 mb-0 text-ink-3">
        Recordings: {ACCEPTED_RECORDINGS}, up to 25 MB. Text: {ACCEPTED_TEXT}. Keep the original until the transcript is saved.
      </p>
      <p className="mt-1.5 mb-0 flex flex-wrap gap-x-3 gap-y-1">
        {phone === "iphone" ? (
          <>
            <Ext href="https://support.apple.com/guide/iphone/export-a-recording-to-files-iph831c37815/ios">Apple: save a voice memo</Ext>
            <Ext href="https://support.apple.com/guide/iphone/record-and-transcribe-a-call-iph57c6590e9/ios">Apple: recorded calls</Ext>
          </>
        ) : (
          <>
            <Ext href="https://support.google.com/pixelphone/answer/16267696">Google: share a recording</Ext>
            <Ext href="https://support.google.com/phoneapp/answer/9803950">Google: recorded calls</Ext>
          </>
        )}
      </p>
    </section>
  );
}
