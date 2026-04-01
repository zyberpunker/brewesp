import { useEffect, useRef } from "react";
import { LineChart } from "echarts/charts";
import {
  DataZoomComponent,
  GridComponent,
  LegendComponent,
  TooltipComponent,
} from "echarts/components";
import { CanvasRenderer } from "echarts/renderers";
import { init, use } from "echarts/core";

import type { TelemetryPoint, TelemetryWindow } from "@/device-detail/types";

type TemperatureChartProps = {
  telemetry: TelemetryPoint[];
  window: TelemetryWindow;
};

use([
  LineChart,
  GridComponent,
  TooltipComponent,
  LegendComponent,
  DataZoomComponent,
  CanvasRenderer,
]);

function formatAxisLabel(
  value: number,
  window: TelemetryWindow,
  latestTimestamp: number | null,
) {
  const latest = latestTimestamp ?? value;
  const spanMs = latest - value;

  if (window === "2h" || (window === "12h" && spanMs < 24 * 60 * 60 * 1000)) {
    return new Intl.DateTimeFormat("sv-SE", {
      hour: "2-digit",
      minute: "2-digit",
      hour12: false,
    }).format(value);
  }

  return new Intl.DateTimeFormat("sv-SE", {
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    hour12: false,
  }).format(value);
}

export function TemperatureChart({ telemetry, window }: TemperatureChartProps) {
  const chartRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    if (!chartRef.current) {
      return;
    }

    const chart = init(chartRef.current, undefined, {
      renderer: "canvas",
    });
    const latestTimestamp =
      telemetry.length > 0
        ? new Date(telemetry[telemetry.length - 1].recorded_at).getTime()
        : null;
    const windowMs =
      window === "2h"
        ? 2 * 60 * 60 * 1000
        : window === "12h"
          ? 12 * 60 * 60 * 1000
          : window === "2d"
            ? 48 * 60 * 60 * 1000
            : null;
    const windowStart =
      latestTimestamp != null && windowMs != null ? latestTimestamp - windowMs : null;

    chart.setOption({
      animationDuration: 300,
      backgroundColor: "transparent",
      grid: { left: 36, right: 18, top: 28, bottom: 56 },
      tooltip: {
        trigger: "axis",
        backgroundColor: "rgba(24, 33, 30, 0.94)",
        borderWidth: 0,
        valueFormatter: (value: string | number) =>
          typeof value === "number" ? `${value.toFixed(1)}\u00B0C` : value,
        axisPointer: {
          animation: false,
        },
        formatter: (
          params: Array<{
            axisValue: number;
            marker: string;
            seriesName: string;
            data: [string, number | null];
          }>,
        ) => {
          if (!params.length) {
            return "";
          }

          const timestamp = params[0].axisValue;
          const header = new Intl.DateTimeFormat("sv-SE", {
            year: "numeric",
            month: "2-digit",
            day: "2-digit",
            hour: "2-digit",
            minute: "2-digit",
            hour12: false,
          }).format(timestamp);

          const lines = params.map((entry) => {
            const rawValue = Array.isArray(entry.data) ? entry.data[1] : null;
            const value =
              typeof rawValue === "number" ? `${rawValue.toFixed(1)}\u00B0C` : "n/a";
            return `${entry.marker}${entry.seriesName}: ${value}`;
          });

          return [header, ...lines].join("<br/>");
        },
        textStyle: {
          color: "#f7f6f2",
          fontFamily: "Manrope, sans-serif",
        },
      },
      legend: {
        top: 0,
        right: 0,
        textStyle: {
          color: "#66736d",
          fontFamily: "Manrope, sans-serif",
        },
      },
      xAxis: {
        type: "time",
        boundaryGap: false,
        min: windowStart ?? "dataMin",
        max: latestTimestamp ?? "dataMax",
        axisLabel: {
          color: "#66736d",
          formatter: (value: number) => formatAxisLabel(value, window, latestTimestamp),
        },
        axisLine: { lineStyle: { color: "rgba(31,39,36,0.12)" } },
      },
      yAxis: {
        type: "value",
        scale: true,
        axisLabel: {
          color: "#66736d",
          formatter: (value: number) => `${value.toFixed(1)}\u00B0`,
        },
        splitLine: { lineStyle: { color: "rgba(31,39,36,0.08)" } },
      },
      dataZoom: [
        {
          type: "inside",
          filterMode: "none",
        },
        {
          type: "slider",
          height: 18,
          bottom: 0,
          borderColor: "rgba(31,39,36,0.08)",
          backgroundColor: "rgba(31,39,36,0.04)",
          fillerColor: "rgba(47,108,96,0.18)",
          handleStyle: {
            color: "#2f6c60",
            borderColor: "#2f6c60",
          },
          moveHandleStyle: {
            color: "#2f6c60",
          },
          textStyle: {
            color: "#66736d",
            fontFamily: "Manrope, sans-serif",
          },
          filterMode: "none",
        },
      ],
      series: [
        {
          name: "Beer",
          type: "line",
          smooth: true,
          showSymbol: false,
          lineStyle: { width: 3, color: "#2f6c60" },
          itemStyle: { color: "#2f6c60" },
          data: telemetry.map((point) => [point.recorded_at, point.temp_primary_c]),
        },
        {
          name: "Target",
          type: "line",
          smooth: true,
          showSymbol: false,
          lineStyle: { width: 2, type: "dashed", color: "#d46a3a" },
          itemStyle: { color: "#d46a3a" },
          data: telemetry.map((point) => [
            point.recorded_at,
            point.effective_target_c ?? point.setpoint_c,
          ]),
        },
        {
          name: "Chamber",
          type: "line",
          smooth: true,
          showSymbol: false,
          lineStyle: { width: 2, color: "#3f86c6", opacity: 0.8 },
          itemStyle: { color: "#3f86c6" },
          data: telemetry.map((point) => [point.recorded_at, point.temp_secondary_c]),
        },
      ],
    });

    const resizeObserver = new ResizeObserver(() => {
      chart.resize();
    });

    resizeObserver.observe(chartRef.current);

    return () => {
      resizeObserver.disconnect();
      chart.dispose();
    };
  }, [telemetry, window]);

  return <div ref={chartRef} className="h-[320px] w-full" />;
}
