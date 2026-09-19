from pathlib import Path
import argparse

import pandas as pd
import matplotlib.pyplot as plt


IMPLEMENTATION_ORDER = ["blocking", "nonblocking", "async"]
TRANSPORT_ORDER = ["tcp", "unix"]

SIZE_LABELS = {
    64: "64 B",
    1024: "1 KB",
    65536: "64 KB",
    1048576: "1 MB",
}


def newest_match(root: Path, exact_name: str, fallback_pattern: str) -> Path:
    exact = list(root.rglob(exact_name))
    if exact:
        return max(exact, key=lambda p: p.stat().st_mtime)

    fallback = list(root.rglob(fallback_pattern))
    if fallback:
        return max(fallback, key=lambda p: p.stat().st_mtime)

    raise FileNotFoundError(
        f"Не знайдено {exact_name}. "
        f"Переконайся, що CSV лежать десь усередині {root}"
    )


def load_result(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)

    numeric_columns = [
        "size", "conns", "count", "secs", "rate_per_s",
        "mb_per_s", "X_us", "Y_us", "Z_us", "failed",
    ]
    for col in numeric_columns:
        df[col] = pd.to_numeric(df[col], errors="coerce")

    return df


def save_line_chart(
    df: pd.DataFrame,
    x_col: str,
    y_col: str,
    title: str,
    x_label: str,
    y_label: str,
    output: Path,
    x_ticks=None,
    x_tick_labels=None,
):
    plt.figure(figsize=(9, 5.5))

    for impl in IMPLEMENTATION_ORDER:
        part = df[df["impl"] == impl].sort_values(x_col)
        if part.empty:
            continue

        plt.plot(
            part[x_col],
            part[y_col],
            marker="o",
            linewidth=2,
            label=impl,
        )

    plt.title(title)
    plt.xlabel(x_label)
    plt.ylabel(y_label)
    plt.grid(True, alpha=0.25)
    plt.legend()

    if x_ticks is not None:
        plt.xticks(x_ticks, x_tick_labels)

    plt.tight_layout()
    plt.savefig(output, dpi=180)
    plt.close()


def plot_connections(echo: pd.DataFrame, transport: str, charts_dir: Path):
    base = echo[
        (echo["transport"] == transport)
        & (echo["size"] == 64)
        & (echo["conns"].isin([1, 4, 16]))
    ]

    save_line_chart(
        base,
        "conns",
        "rate_per_s",
        f"Продуктивність за кількістю з'єднань — {transport.upper()}, 64 B",
        "Кількість одночасних з'єднань",
        "Повідомлень/с",
        charts_dir / f"messages_vs_connections_{transport}_64B.png",
        x_ticks=[1, 4, 16],
        x_tick_labels=["1", "4", "16"],
    )

    save_line_chart(
        base,
        "conns",
        "X_us",
        f"Середній RTT за кількістю з'єднань — {transport.upper()}, 64 B",
        "Кількість одночасних з'єднань",
        "RTT, мкс",
        charts_dir / f"rtt_vs_connections_{transport}_64B.png",
        x_ticks=[1, 4, 16],
        x_tick_labels=["1", "4", "16"],
    )


def plot_throughput_by_size(echo: pd.DataFrame, transport: str, charts_dir: Path):
    sizes = [64, 1024, 65536, 1048576]

    base = echo[
        (echo["transport"] == transport)
        & (echo["conns"] == 1)
        & (echo["size"].isin(sizes))
    ].copy()

    base["size_order"] = base["size"].map({size: i for i, size in enumerate(sizes)})

    plt.figure(figsize=(9, 5.5))

    for impl in IMPLEMENTATION_ORDER:
        part = base[base["impl"] == impl].sort_values("size_order")
        if part.empty:
            continue

        plt.plot(
            part["size_order"],
            part["mb_per_s"],
            marker="o",
            linewidth=2,
            label=impl,
        )

    plt.title(
        f"Пропускна здатність залежно від розміру повідомлення — "
        f"{transport.upper()}, 1 з'єднання"
    )
    plt.xlabel("Розмір повідомлення")
    plt.ylabel("MB/s")
    plt.xticks(
        range(len(sizes)),
        [SIZE_LABELS[s] for s in sizes],
    )
    plt.grid(True, alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(
        charts_dir / f"throughput_vs_message_size_{transport}_1conn.png",
        dpi=180,
    )
    plt.close()


def plot_tcp_vs_unix(echo: pd.DataFrame, charts_dir: Path):
    base = echo[
        (echo["size"] == 64)
        & (echo["conns"] == 1)
        & (echo["transport"].isin(TRANSPORT_ORDER))
    ]

    pivot = (
        base.pivot_table(
            index="impl",
            columns="transport",
            values="rate_per_s",
            aggfunc="first",
        )
        .reindex(IMPLEMENTATION_ORDER)
    )

    x = list(range(len(pivot.index)))
    width = 0.36

    plt.figure(figsize=(9, 5.5))

    for idx, transport in enumerate(TRANSPORT_ORDER):
        if transport not in pivot.columns:
            continue

        offsets = [
            value + (idx - 0.5) * width
            for value in x
        ]

        plt.bar(
            offsets,
            pivot[transport].values,
            width=width,
            label=transport.upper(),
        )

    plt.title("TCP vs AF_UNIX — 64 B, 1 з'єднання")
    plt.xlabel("Режим I/O")
    plt.ylabel("Повідомлень/с")
    plt.xticks(x, pivot.index)
    plt.grid(True, axis="y", alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(charts_dir / "tcp_vs_unix_64B_1conn.png", dpi=180)
    plt.close()


def plot_connect_overhead(conn: pd.DataFrame, charts_dir: Path):
    if conn.empty:
        return

    base = conn[conn["conns"] == 1].copy()
    if base.empty:
        return

    base["category"] = base["impl"] + " / " + base["transport"]

    plt.figure(figsize=(10, 5.8))
    plt.bar(base["category"], base["Y_us"])
    plt.title("Середній час connect() — додаткова метрика")
    plt.xlabel("Режим / транспорт")
    plt.ylabel("connect(), мкс")
    plt.xticks(rotation=30, ha="right")
    plt.grid(True, axis="y", alpha=0.25)
    plt.tight_layout()
    plt.savefig(charts_dir / "connect_overhead.png", dpi=180)
    plt.close()


def make_summary_tables(all_results: pd.DataFrame, output_dir: Path):
    echo = all_results[
        (all_results["mode"] == "echo")
        & (all_results["failed"] == 0)
    ].copy()

    conn = all_results[
        (all_results["mode"] == "conn")
        & (all_results["failed"] == 0)
        & (all_results["conns"] == 1)
    ].copy()

    echo_summary = echo[
        [
            "impl", "transport", "size", "conns",
            "rate_per_s", "mb_per_s", "X_us",
        ]
    ].rename(columns={"X_us": "rtt_us"})

    conn_summary = conn[
        [
            "impl", "transport", "conns",
            "rate_per_s", "X_us", "Y_us", "Z_us",
        ]
    ].rename(
        columns={
            "rate_per_s": "connections_per_s",
            "X_us": "socket_us",
            "Y_us": "connect_us",
            "Z_us": "close_us",
        }
    )

    echo_summary.to_csv(
        output_dir / "summary_echo.csv",
        index=False,
        encoding="utf-8-sig",
    )

    conn_summary.to_csv(
        output_dir / "summary_connection.csv",
        index=False,
        encoding="utf-8-sig",
    )

    return echo, conn


def print_hypothesis_evidence(echo: pd.DataFrame):
    print("\n=== Дані для перевірки гіпотез ===")

    h1 = echo[
        (echo["size"] == 64)
        & (echo["conns"] == 1)
    ][["impl", "transport", "rate_per_s", "X_us"]]

    print("\nH1: AF_UNIX vs TCP, 64 B, 1 connection")
    print(h1.to_string(index=False))

    h2 = echo[
        (echo["transport"] == "tcp")
        & (echo["size"] == 64)
        & (echo["conns"].isin([1, 16]))
    ][["impl", "conns", "rate_per_s", "X_us"]]

    print("\nH2/H3: TCP, 64 B, 1 vs 16 connections")
    print(h2.to_string(index=False))

    h4 = echo[
        (echo["transport"] == "tcp")
        & (echo["conns"] == 1)
        & (echo["size"].isin([64, 65536, 1048576]))
    ][["impl", "size", "rate_per_s", "mb_per_s", "X_us"]]

    print("\nH4: TCP, 1 connection, різні розміри")
    print(h4.to_string(index=False))


def main():
    parser = argparse.ArgumentParser(
        description="Аналіз benchmark результатів blocking/non-blocking/async sockets."
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent,
        help=(
            "Коренева папка лабораторної. "
            "За замовчуванням — папка, де лежить цей script."
        ),
    )
    args = parser.parse_args()

    root = args.root.resolve()

    paths = {
        "blocking": newest_match(
            root,
            "results_blocking.csv",
            "results_blocking*.csv",
        ),
        "nonblocking": newest_match(
            root,
            "results_nonblocking.csv",
            "results_nonblocking*.csv",
        ),
        "async": newest_match(
            root,
            "results_async.csv",
            "results_async*.csv",
        ),
    }

    print("Використовую CSV:")
    for impl, path in paths.items():
        print(f"  {impl:12s}: {path}")

    frames = []

    for expected_impl, path in paths.items():
        df = load_result(path)

        if "impl" not in df.columns:
            raise ValueError(f"У {path} немає колонки impl")

        frames.append(df)

    all_results = pd.concat(frames, ignore_index=True)

    output_dir = root / "results"
    charts_dir = output_dir / "charts"

    output_dir.mkdir(parents=True, exist_ok=True)
    charts_dir.mkdir(parents=True, exist_ok=True)

    all_results.to_csv(
        output_dir / "combined_results.csv",
        index=False,
        encoding="utf-8-sig",
    )

    echo, conn = make_summary_tables(
        all_results,
        output_dir,
    )

    plot_connections(echo, "tcp", charts_dir)
    plot_connections(echo, "unix", charts_dir)

    plot_throughput_by_size(echo, "tcp", charts_dir)
    plot_throughput_by_size(echo, "unix", charts_dir)

    plot_tcp_vs_unix(echo, charts_dir)
    plot_connect_overhead(conn, charts_dir)

    print_hypothesis_evidence(echo)

    print("\n=== Готово ===")
    print(f"Зведені результати: {output_dir / 'combined_results.csv'}")
    print(f"Таблиця echo:       {output_dir / 'summary_echo.csv'}")
    print(f"Таблиця connect:    {output_dir / 'summary_connection.csv'}")
    print(f"Графіки:            {charts_dir}")


if __name__ == "__main__":
    main()
