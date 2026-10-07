#!/usr/bin/env python3
"""
Generate realistic test datasets (CSV and SQLite) for DataTableViewer testing.
Default generates 100,000 rows with mixed data types, CJK text, and long cells.
"""

import argparse
import csv
import os
import random
import sqlite3
import sys
import time
from datetime import datetime, timedelta

CATEGORIES = [
    "Electronics", "Books", "Office Supplies", "Home & Kitchen",
    "Sports", "Clothing", "数码配件", "图书音像", "食品生鲜", "家居日用"
]

STATUSES = ["Active", "Pending", "Completed", "Cancelled", "Processing"]

EN_FIRST_NAMES = [
    "James", "Mary", "John", "Patricia", "Robert", "Jennifer", "Michael", "Linda",
    "William", "Elizabeth", "David", "Barbara", "Richard", "Susan", "Joseph", "Jessica"
]

EN_LAST_NAMES = [
    "Smith", "Johnson", "Williams", "Brown", "Jones", "Garcia", "Miller", "Davis",
    "Rodriguez", "Martinez", "Hernandez", "Lopez", "Gonzalez", "Wilson", "Anderson"
]

ZH_FIRST_NAMES = ["伟", "芳", "强", "洋", "杰", "敏", "磊", "丽", "晨", "婷", "宇", "静"]
ZH_LAST_NAMES = ["李", "王", "张", "刘", "陈", "杨", "赵", "黄", "周", "吴", "徐", "孙"]

DOMAINS = ["example.com", "test.org", "corp.net", "seer.dev", "data.io"]


def random_date(start_date: datetime, end_date: datetime) -> str:
    delta = end_date - start_date
    int_delta = int(delta.total_seconds())
    random_second = random.randint(0, int_delta)
    dt = start_date + timedelta(seconds=random_second)
    return dt.strftime("%Y-%m-%d %H:%M:%S")


def generate_batch(start_id: int, count: int, start_date: datetime, end_date: datetime, include_long_text: bool):
    rows = []
    for i in range(count):
        row_id = start_id + i
        if random.random() < 0.3:
            # 30% CJK names
            full_name = f"{random.choice(ZH_LAST_NAMES)}{random.choice(ZH_FIRST_NAMES)}"
        else:
            full_name = f"{random.choice(EN_FIRST_NAMES)} {random.choice(EN_LAST_NAMES)}"

        email = f"user_{row_id}@{random.choice(DOMAINS)}"
        category = random.choice(CATEGORIES)
        amount = round(random.uniform(1.5, 9999.9), 2)
        score = random.randint(1, 100)
        status = random.choice(STATUSES)
        created_at = random_date(start_date, end_date)

        # Occasional special notes for testing edge cases
        if include_long_text and row_id % 2000 == 0:
            # Over 4096 bytes to test cell clamping and async refetching in SQLite
            notes = f"[Long Text Clamped #{row_id}] " + ("测试超长文本数据_Over4096Bytes_" * 150)
        elif row_id % 500 == 0:
            # Embedded newlines and quotes to test CSV escaping and singleLineDisplayText
            notes = f"Note line 1\nNote line 2 with \"quotes\", and commas\r\nNote line 3 for #{row_id}"
        elif row_id % 300 == 0:
            notes = f"Special chars: <tag>&amp; | markdown pipe | #{row_id}"
        else:
            notes = f"Standard record #{row_id}"

        rows.append((row_id, full_name, email, category, amount, score, status, created_at, notes))
    return rows


def generate_csv(file_path: str, total_rows: int, batch_size: int, include_long_text: bool):
    print(f"[*] Generating CSV ({total_rows:,} rows) -> {file_path}")
    start_time = time.time()
    start_date = datetime(2023, 1, 1)
    end_date = datetime(2026, 10, 1)

    headers = [
        "id", "name", "email", "category", "amount",
        "score", "status", "created_at", "notes"
    ]

    os.makedirs(os.path.dirname(os.path.abspath(file_path)), exist_ok=True)
    with open(file_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(headers)

        rows_written = 0
        while rows_written < total_rows:
            current_batch = min(batch_size, total_rows - rows_written)
            batch = generate_batch(rows_written + 1, current_batch, start_date, end_date, include_long_text)
            writer.writerows(batch)
            rows_written += current_batch

    elapsed = time.time() - start_time
    file_size_mb = os.path.getsize(file_path) / (1024 * 1024)
    print(f"    [+] CSV done: {total_rows:,} rows, {file_size_mb:.2f} MB in {elapsed:.2f}s")


def generate_sqlite(file_path: str, total_rows: int, batch_size: int, include_long_text: bool):
    print(f"[*] Generating SQLite ({total_rows:,} rows) -> {file_path}")
    start_time = time.time()
    start_date = datetime(2023, 1, 1)
    end_date = datetime(2026, 10, 1)

    if os.path.exists(file_path):
        os.remove(file_path)

    os.makedirs(os.path.dirname(os.path.abspath(file_path)), exist_ok=True)
    conn = sqlite3.connect(file_path)
    cur = conn.cursor()

    # Fast insertion settings
    cur.execute("PRAGMA journal_mode = MEMORY;")
    cur.execute("PRAGMA synchronous = OFF;")

    # 1. Main items table
    cur.execute("""
        CREATE TABLE items (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL,
            email TEXT,
            category TEXT,
            amount REAL,
            score INTEGER,
            status TEXT,
            created_at TEXT,
            notes TEXT
        );
    """)

    # 2. Summary stats table (for multi-table picker testing in DataTableViewer)
    cur.execute("""
        CREATE TABLE categories_summary (
            category TEXT PRIMARY KEY,
            item_count INTEGER,
            avg_amount REAL,
            max_amount REAL
        );
    """)

    sql_insert = """
        INSERT INTO items (id, name, email, category, amount, score, status, created_at, notes)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    """

    rows_written = 0
    while rows_written < total_rows:
        current_batch = min(batch_size, total_rows - rows_written)
        batch = generate_batch(rows_written + 1, current_batch, start_date, end_date, include_long_text)
        cur.executemany(sql_insert, batch)
        rows_written += current_batch

    # Populate summary table
    cur.execute("""
        INSERT INTO categories_summary (category, item_count, avg_amount, max_amount)
        SELECT category, COUNT(*), ROUND(AVG(amount), 2), MAX(amount)
        FROM items
        GROUP BY category;
    """)

    # Create an index on category and created_at to verify query optimizer interaction
    cur.execute("CREATE INDEX idx_items_category ON items(category);")
    cur.execute("CREATE INDEX idx_items_created ON items(created_at);")

    conn.commit()
    conn.close()

    elapsed = time.time() - start_time
    file_size_mb = os.path.getsize(file_path) / (1024 * 1024)
    print(f"    [+] SQLite done: {total_rows:,} rows, {file_size_mb:.2f} MB in {elapsed:.2f}s (Tables: items, categories_summary)")


def main():
    parser = argparse.ArgumentParser(description="Generate 100k-row CSV and SQLite test datasets for DataTableViewer.")
    parser.add_argument("--rows", type=int, default=100000, help="Total number of rows to generate (default: 100000)")
    parser.add_argument("--batch-size", type=int, default=10000, help="Batch size for writes (default: 10000)")
    parser.add_argument("--output-dir", type=str, default="./data", help="Output directory (default: ./data)")
    parser.add_argument("--csv-name", type=str, default="test_100k.csv", help="Output CSV filename")
    parser.add_argument("--db-name", type=str, default="test_100k.sqlite", help="Output SQLite filename")
    parser.add_argument("--no-long-text", action="store_true", help="Do not include >4096-byte long text cells")

    args = parser.parse_args()

    random.seed(42)  # Deterministic seed for reproducible tests

    csv_path = os.path.join(args.output_dir, args.csv_name)
    sqlite_path = os.path.join(args.output_dir, args.db_name)

    print(f"=== Generating Test Data ({args.rows:,} rows) ===")
    print(f"Target directory: {os.path.abspath(args.output_dir)}")

    total_start = time.time()
    generate_csv(csv_path, args.rows, args.batch_size, not args.no_long_text)
    generate_sqlite(sqlite_path, args.rows, args.batch_size, not args.no_long_text)
    total_elapsed = time.time() - total_start

    print(f"\nAll datasets generated successfully in {total_elapsed:.2f}s!")
    print(f"  CSV:    {os.path.abspath(csv_path)}")
    print(f"  SQLite: {os.path.abspath(sqlite_path)}")


if __name__ == "__main__":
    main()
