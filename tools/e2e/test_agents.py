#!/usr/bin/env python3
"""Offline checks of agents.py's own logic, with no Lake: what T3 ranks accounts by (lake#1258).

    python3 tools/e2e/test_agents.py

The workspace's SQL answers are stubbed, so this runs anywhere.
"""
import os
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agents  # noqa: E402

UPLOADED = "SELECT account, risk FROM datasets.ux_review_at_risk ORDER BY risk DESC LIMIT 3"
# system.datasets as lake-dev answered it on 2026-10-27 after the Quickstart's film run, and as
# lake-testing had it in lake#1258: the run's at_risk table has the films' columns, no account.
FILMS = {"database": "churn_results", "dataset": "at_risk_d4446d16",
         "columns": "dominant_driver, driver_effect, genre=comedy, genre=romance, rank, rating, "
                    "reason_codes, risk, title"}
ACCOUNTS = {"database": "churn_results", "dataset": "at_risk_0a1b2c3d", "columns": "account, rank, risk"}


class Ranking(unittest.TestCase):
    def prepare_with(self, tables):
        """prepare() over a workspace whose datasets are present and whose system.datasets also
        lists `tables`."""
        listed = [{"database": "datasets", "dataset": "ux_review_churn", "columns": "account, churned"},
                  {"database": "datasets", "dataset": "ux_review_at_risk", "columns": "account, risk"},
                  {"database": "datasets", "dataset": "ux_review_churn_study", "columns": "account_id, churned"},
                  *tables]

        def sql_rows(statement):
            if "system.datasets" in statement:
                return listed
            raise AssertionError(f"unexpected SQL: {statement}")

        saved = agents.sql_rows
        agents.sql_rows = sql_rows
        try:
            return agents.prepare()
        finally:
            agents.sql_rows = saved

    def test_a_film_table_is_not_ranked_by_account(self):
        self.assertEqual(self.prepare_with([FILMS]), [UPLOADED])

    def test_a_table_with_accounts_is_ranked(self):
        self.assertEqual(self.prepare_with([FILMS, ACCOUNTS]),
                         ["SELECT account, risk FROM churn_results.at_risk_0a1b2c3d ORDER BY risk DESC LIMIT 3"])

    def test_every_table_with_accounts_is_an_answer(self):
        other = {**ACCOUNTS, "dataset": "at_risk_ffff0000"}
        self.assertEqual(len(self.prepare_with([ACCOUNTS, other])), 2)

    def test_without_runs_the_uploaded_risk_is_used(self):
        self.assertEqual(self.prepare_with([]), [UPLOADED])

    def test_columns_are_read_from_a_string_or_a_list(self):
        self.assertEqual(agents.columns_of({"columns": "account, risk"}), {"account", "risk"})
        self.assertEqual(agents.columns_of({"columns": ["account", "risk"]}), {"account", "risk"})
        self.assertEqual(agents.columns_of({}), set())


if __name__ == "__main__":
    unittest.main()
