import type { HTMLAttributes } from "react";

import { cn } from "@/lib/utils";

const toneClasses: Record<string, string> = {
  online: "bg-emerald-100 text-emerald-800 ring-emerald-800/10",
  stale: "bg-amber-100 text-amber-800 ring-amber-800/10",
  offline: "bg-rose-100 text-rose-800 ring-rose-800/10",
  heating: "bg-[color:color-mix(in_srgb,var(--heat)_18%,white)] text-[var(--heat)] ring-[color:color-mix(in_srgb,var(--heat)_18%,black)]",
  cooling: "bg-[color:color-mix(in_srgb,var(--cool)_18%,white)] text-[var(--cool)] ring-[color:color-mix(in_srgb,var(--cool)_18%,black)]",
  neutral: "bg-black/5 text-[var(--muted)] ring-black/8",
  fault: "bg-[color:color-mix(in_srgb,var(--fault)_14%,white)] text-[var(--fault)] ring-[color:color-mix(in_srgb,var(--fault)_14%,black)]",
};

type BadgeProps = HTMLAttributes<HTMLSpanElement> & {
  tone?: keyof typeof toneClasses;
};

export function Badge({
  className,
  tone = "neutral",
  ...props
}: BadgeProps) {
  return (
    <span
      className={cn(
        "inline-flex min-h-8 items-center gap-1.5 rounded-full px-3 text-[11px] font-bold uppercase tracking-[0.16em] ring-1",
        toneClasses[tone],
        className,
      )}
      {...props}
    />
  );
}
