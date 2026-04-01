import { cva, type VariantProps } from "class-variance-authority";
import type { ButtonHTMLAttributes } from "react";

import { cn } from "@/lib/utils";

const buttonVariants = cva(
  "inline-flex min-h-11 items-center justify-center gap-2 rounded-full px-4 text-sm font-semibold transition duration-150 disabled:cursor-not-allowed disabled:opacity-50",
  {
    variants: {
      variant: {
        primary:
          "bg-[var(--accent)] text-white shadow-[0_10px_24px_rgba(47,108,96,0.18)] hover:bg-[color-mix(in_srgb,var(--accent)_90%,black)]",
        secondary:
          "bg-white/80 text-[var(--ink)] ring-1 ring-black/8 hover:bg-white",
        subtle:
          "bg-[var(--surface-strong)] text-[var(--ink)] hover:bg-[color-mix(in_srgb,var(--surface-strong)_88%,black)]",
        danger:
          "bg-[var(--fault)] text-white shadow-[0_10px_24px_rgba(178,65,59,0.16)] hover:bg-[color-mix(in_srgb,var(--fault)_92%,black)]",
      },
      tone: {
        neutral: "",
        heat: "data-[active=true]:bg-[var(--heat)] data-[active=true]:text-white",
        cool: "data-[active=true]:bg-[var(--cool)] data-[active=true]:text-white",
      },
    },
    defaultVariants: {
      variant: "secondary",
      tone: "neutral",
    },
  },
);

type ButtonProps = ButtonHTMLAttributes<HTMLButtonElement> &
  VariantProps<typeof buttonVariants>;

export function Button({ className, variant, tone, ...props }: ButtonProps) {
  return (
    <button
      className={cn(buttonVariants({ variant, tone }), className)}
      {...props}
    />
  );
}
