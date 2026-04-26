# Testing strategies (Smart Library Table Reservation & IoT monitoring)

Testing verified core behaviour: **reservations and business rules**, **authentication and roles**, **public map/APIs**, **admin console**, and **IoT/directory** admin APIs. Layout matches our reference format (expected vs actual vs status). **All cases below: Passed.**

---

## 1. User reservations (highest priority)

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Valid booking: inside 09:00–18:00 (MYT), 30-minute slots, no overlap | Created; OTP + email; DB matches UI | Created; email received; OTP stored | Passed |
| 2 | Misaligned times, outside hours, or overlapping same table | Validation / overlap rejection | Errors returned; no bad rows | Passed |
| 3 | Daily total > 240 minutes same calendar day | Cap rejected with clear error | Cap enforced | Passed |
| 4 | Email send failure on create | Rollback; user error; no orphan reservation | Rolled back; error shown | Passed |
| 5 | “My reservations” as non-admin; admin hits user booking API | Own list only; admin gets 403 | List scoped; 403 for admin | Passed |

---

## 2. Sign up and authentication

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Wrong password / empty fields | No JWT; error shown | No tokens; validation or API error | Passed |
| 2 | Non-admin vs admin login | Role-correct redirect (`/reservations` or home vs `/admin`) | Redirects matched role | Passed |
| 3 | Non-admin opens `/admin/...` | Guard + admin APIs 403 | Redirect / 403 | Passed |
| 4 | Sign-up: valid public role vs admin / invalid role | User created hashed; bad role rejected | Created / rejected as expected | Passed |

---

## 3. Public library map and public APIs

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Home without login; `GET /api/tables/` | Map + table geometry/status | Loaded; JSON correct | Passed |
| 2 | `GET /api/map-reservations/` | Slots for map; no private user PII | Anonymized payload | Passed |
| 3 | Weight-availability URL (valid / invalid table) | Sensor + end times when active; 404 if missing | Both cases correct | Passed |

---

## 4. Admin console (dashboard, tables, reservations)

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Admin home + sidebar routes | Dashboard loads; links hit correct admin pages | All routes OK | Passed |
| 2 | Tables map: layout, status colours, metadata edits | Matches DB; PATCH persists; public map updates | Grid and API consistent | Passed |
| 3 | Reservation list/detail/filter; non-admin on admin APIs | List/detail OK; 403 for non-admin | Worked; 403 enforced | Passed |
| 4 | Expired session / cleared token | APIs 401/403; UI to login | Handled | Passed |

---

## 5. IoT admin and user directory

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Weight sensors & LCD lists; create/edit validation | Columns correct; invalid data rejected; LCD one-to-one table | Passed all checks | Passed |
| 2 | Non-admin on `/api/admin/...` IoT routes | 403 | 403 | Passed |
| 3 | Directory by role: search, order whitelist, single admin rule | Filtered lists; safe ordering; second admin blocked | Lists and search OK; ordering safe; second admin rejected | Passed |

---

## Summary

**Reservations** (hours, slots, overlap, daily cap, email/OTP rollback) and **access control** (JWT, admin vs library user) were validated first, then **public map feeds**, **admin** tables and booking oversight, and finally **IoT** and **directory** admin endpoints. **Actual results matched expected results** for every row above (**Passed**).
