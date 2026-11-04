---
title: Environments and read-only
---

# Environments and read-only

Two settings in the connection dialog decide how carefully Lume treats a connection. They are yours to set: Lume never guesses the environment from the host name.

## Environment: DEV, STG, PROD

| Label | Meaning |
|---|---|
| None | no label |
| **DEV** | development |
| **STG** | staging (pre-production). Connections saved with the older label HML are read as STG |
| **PROD** | production |

The label appears as a pill on the toolbar's datasource button, in the connection chooser, in **Switch Tab** and in every write confirmation.

### What PROD changes

- **Writes are confirmed.** On PROD, SQL that may change data or schema, or a script of several statements, opens a confirmation that shows the full target (connection, schema, session). **Enter never confirms**; **Cancel** runs nothing. The check is deliberately cautious and the dialog says so: the server's own permissions are what finally decide.
- **Re-run Last** shows the target and the SQL before running.
- **Table Designer** and **object editor** ask you to type the table or object name before applying a change.
- **Several objects at once** cannot be applied on PROD: each object goes through its own review.
- **Explain Analyze** of a statement that writes is refused.
- Choosing PROD in the dialog turns **Read-only in Lume** on. You can turn it off again.

## Read-only in Lume

With **Read-only in Lume** checked, Lume refuses to run writes on the connection: DML, DDL, data edits in the grid, Table Designer and object changes. A **Read-only** pill appears on the toolbar; its tooltip says how strong the protection is for that database:

- enforced by the engine itself where it can be (SQLite);
- set as the session's default where the server supports it (PostgreSQL);
- otherwise checked by Lume before sending.

When Lume checks before sending, it reads the statement's first keyword, and for `WITH` queries it looks for `INSERT`, `UPDATE`, `DELETE` or `MERGE` used as SQL. Words inside strings, quoted names and comments do not count, so a query that only lists `'...:UPDATE'` as text runs normally. The check is cautious: when in doubt, it treats the statement as a write.

The database login is not changed. For the strongest protection, also use a login that only has read permissions.

When a write is refused, the **Output** says who refused it: Lume's read-only setting, the server's privileges, or a read-only setting on the server. A statement Lume refuses before sending it also appears there, as a **skipped** execution with the SQL and the reason, so the message stays after the status bar changes.

## Settings that always apply

**Settings > Query Execution > Protection** summarises these rules. They cannot be switched off globally: they follow each connection's environment and read-only setting.
