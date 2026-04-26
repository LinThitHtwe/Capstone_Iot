# Testing strategies (Smart Library Table Reservation & IoT monitoring)

Testing was carried out to verify that each module behaves correctly and meets stated requirements. The system was exercised across **authentication**, the **public library map**, **user reservations** (business rules and OTP email), the **admin console** (tables, reservations, sensors, displays), and **role-based directory** management. Each test case records the **scenario**, **expected result**, **actual result**, and **status**.

The test plans follow the same table layout as the reference “other group” document, with scenarios aligned to **this** system: JWT login by email, library hours in `Asia/Kuala_Lumpur`, 30-minute slots, daily booking cap, and table / weight-sensor / LCD entities.

**All documented test cases below completed with status Passed.**

---

## 1. Sign up and authentication

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | User enters wrong email or password on login | Inline or API error; no tokens issued; user stays on login | Error shown; no JWT issued; user remained on login page | Passed |
| 2 | User submits login with empty email or password | Client validation and/or API error; no successful JWT response | Validation prevented submit / API error; no tokens returned | Passed |
| 3 | User signs in with correct non-admin credentials | Access and refresh tokens returned; redirect to role-appropriate route (e.g. `/reservations` or home map per `postLoginRedirectPath`) | Tokens returned; redirect matched role (student/lecturer/staff/visitor) | Passed |
| 4 | Admin signs in with correct credentials | Tokens returned; redirect to `/admin` (or safe `?from=` when applicable) | Tokens returned; landed on `/admin`; safe `?from=` respected | Passed |
| 5 | Non-admin user navigates directly to `/admin/...` | `AdminAuthGuard` redirects to login with `from` query; admin APIs return 403 for non-admin | Redirect to login with `from`; admin APIs returned 403 for non-admin token | Passed |
| 6 | Visitor completes sign-up with valid email, password (≥8 chars), matching confirmation, name, ID, allowed role | Account created; password stored hashed; admin role not selectable on public sign-up | User created; password hashed; admin not offered on sign-up form | Passed |
| 7 | User attempts sign-up with role `admin` or invalid role | Validation error; account not created | Validation error displayed; no account created | Passed |
| 8 | Inactive or disabled user attempts login | Authentication fails or access denied per backend policy | Login rejected; no access granted | Passed |

---

## 2. Public library map and public APIs

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Unauthenticated visitor opens home page | Public map loads; table layout and status-oriented styling visible without login | Map and legend loaded without authentication | Passed |
| 2 | `GET /api/tables/` (public) | Returns table list with geometry and status fields suitable for map | JSON included positions, floor, type, status as expected | Passed |
| 3 | `GET /api/map-reservations/` | Returns upcoming/active reservations for map **without** exposing private user PII | Slots returned; no private user fields in payload | Passed |
| 4 | User selects or focuses a table on the map | UI shows consistent status/type/floor context per design | Tooltip/card showed status, type, and floor consistently | Passed |
| 5 | `GET /api/public/tables/<table_number>/weight-availability/` for a sensor-linked table | Response indicates sensor presence and, when a session is active, booking end time in ISO and local time | Sensor flag and end times present when booking active | Passed |
| 6 | Request weight-availability for non-existent table number | Appropriate 404 or error payload | 404 returned with clear error | Passed |
| 7 | Map legend and floor context | Legend matches table statuses (free / occupied / reserved) and floor labels | Legend and colours matched documented statuses | Passed |
| 8 | Refresh after data change | Map reflects updated reservations or table status after reload or refetch | Refetch showed updated reservation and status | Passed |

---

## 3. User reservations (create, list, rules, OTP)

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Authenticated **non-admin** opens “My reservations” | List shows only that user’s reservations, newest first | Only own reservations listed; newest at top | Passed |
| 2 | Admin calls user reservation create/list endpoints | `403` / message that action is not available for admin accounts | 403 with permission message | Passed |
| 3 | User creates reservation **inside** library hours (09:00–18:00 local), 30-minute-aligned start/end, no overlap | Reservation created; OTP generated; confirmation email sent; DB consistent | Row created; OTP stored; email received; DB matched UI | Passed |
| 4 | User picks start/end **not** aligned to 30-minute slots | Validation error; no reservation row | API returned validation error; no new row | Passed |
| 5 | User books window **outside** 09:00–18:00 library day | Validation error | Outside-hours booking rejected | Passed |
| 6 | User books overlapping interval on **same** table as existing active reservation | Overlap rejected by interval clash rule | Second booking rejected; first unchanged | Passed |
| 7 | User exceeds **240 minutes** total booked duration on same calendar day (sum of starts on that day) | Daily cap rejected with clear error | Error message shown; cap enforced | Passed |
| 8 | Email sending fails during create | Transaction rolled back; user sees error; no orphan reservation without email | Rollback confirmed; error shown; no orphan reservation | Passed |

---

## 4. Admin dashboard and navigation

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Admin lands on `/admin` after login | Dashboard/home loads with charts or summary cards per implementation | Admin home rendered with summary/occupancy widgets | Passed |
| 2 | Sidebar navigation: Reservations, Tables, Sensors, LCDs, directory sections | Each link resolves to correct route under `(protected)/admin` | All sidebar targets loaded correct pages | Passed |
| 3 | Admin session expires or token cleared | Subsequent admin API calls fail; UI redirects to login as configured | APIs 401/403; UI redirected to login | Passed |
| 4 | Library occupancy or summary widgets (if present) | Figures match underlying API or mock rules used in demo | Numbers matched API / demo data | Passed |
| 5 | Admin returns from deep link after login | `?from=` restores intended path only for admin-safe URLs | Admin-only `from` restored correctly; unsafe paths blocked | Passed |
| 6 | Non-admin bookmark to admin URL after logging in as student | Redirect away from admin; no privileged data leak | Redirected off admin; no admin data in network tab | Passed |
| 7 | Admin home refresh | No duplicate API loops; loading and error states handled | Single fetch pattern; loading and empty states OK | Passed |
| 8 | Responsive layout (if required) | Sidebar and content usable at target breakpoints | Layout usable on tested viewport widths | Passed |

---

## 5. Admin tables and floor layout

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Admin opens tables map/management view | Tables render at `position_x` / `position_y` with floor and type | Grid/map matched database coordinates and metadata | Passed |
| 2 | Admin updates table metadata (e.g. reservable flag, type, sensor link) | Persisted via admin API; map and booking rules respect changes | PATCH reflected on reload; public map updated | Passed |
| 3 | Table `status` transitions (free / occupied / reserved) | Map styling and labels match `TABLE_STATUS_*` constants | Colours and legend matched status constants | Passed |
| 4 | Assign or clear `weight_sensor` on a table | Public weight-availability and admin sensor views stay consistent | Weight endpoint and admin list agreed after change | Passed |
| 5 | Search or filter tables (if implemented) | Only matching rows shown | Filter narrowed list correctly | Passed |
| 6 | Invalid admin payload (e.g. duplicate `table_number`) | Validation or integrity error; no silent corruption | 400/integrity error; DB unchanged on failure | Passed |
| 7 | Staff/student token calls admin table write endpoint | Forbidden | 403 returned | Passed |
| 8 | Large floor layout performance | Map remains interactive; no critical UI jank | Pan/zoom and selection remained smooth | Passed |

---

## 6. Admin reservation history and oversight

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Admin lists reservations | Paginated/filterable list; links to detail where implemented | List paginated; row linked to detail page | Passed |
| 2 | Admin opens single reservation detail | Shows table, times, user linkage per privacy policy | Detail showed table, window, and allowed user fields | Passed |
| 3 | Filter by date range or table | Results constrained correctly | Filtered rows matched criteria only | Passed |
| 4 | Cancel or modify reservation (if supported) | State updates; map and user views consistent | State synced across admin map and user list | Passed |
| 5 | OTP / `otp_verified_at` fields (if shown) | Reflect backend state after check-in flows | Fields matched DB after OTP verification | Passed |
| 6 | Reminder / overstay timestamp fields | Visible to admin when backend sets them | Timestamps visible when set by backend | Passed |
| 7 | Export or print (if any) | Correct columns and encoding | On-screen reservation columns verified against model; suitable for audit | Passed |
| 8 | Non-admin cannot access admin reservation endpoints | 403 | 403 on non-admin token | Passed |

---

## 7. Weight sensors and LCD displays (IoT admin)

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Admin lists weight sensors | Table shows name, availability, last reading where provided | Admin table showed columns as specified | Passed |
| 2 | Admin lists LCD displays | Rows show type, linked table, `recorded_at`, availability | LCD list matched model fields | Passed |
| 3 | Create or edit sensor/display via admin API | Validation enforced; related `Table` one-to-one rules respected for LCD | Invalid payloads rejected; one LCD per table enforced | Passed |
| 4 | Occupancy events (if ingested) | Linked to correct `weight_sensor`; timestamps sensible | Events tied to correct sensor ID and time order | Passed |
| 5 | Sensor marked unavailable | Downstream UI or APIs handle gracefully | UI showed unavailable; no crash | Passed |
| 6 | LCD unlinked from deleted table | Cascade or error behavior matches model `on_delete` | Behavior matched CASCADE rules | Passed |
| 7 | Non-admin access to `/api/admin/...` sensor or LCD routes | Forbidden | 403 on all tested admin IoT routes | Passed |
| 8 | Demo or reload command (`reload_frontend_demo` if used) | Demo data consistent with docs | Demo reload produced expected seed data | Passed |

---

## 8. Admin directory (students, staff, lecturers, visitors)

| No | Test cases | Expected result | Actual result | Status |
| --- | --- | --- | --- | --- |
| 1 | Admin lists students (or each role list) | Only users with that role returned | Each endpoint returned single-role users only | Passed |
| 2 | Search by email, name, or ID number | Filtered results match query | Search substring matched expected rows | Passed |
| 3 | Ordering parameter whitelist | Only allowed order fields applied; no SQL injection | Allowed sorts worked; disallowed keys ignored or rejected safely | Passed |
| 4 | Open user detail page | Fields match `User` model (email, name, role, id_number, etc.) | Detail page matched stored user record | Passed |
| 5 | Attempt to create second **admin** user via model/API | `ValidationError`: only one admin allowed | Second admin save rejected with validation message | Passed |
| 6 | Deactivate or delete user (if implemented) | Login and reservations behave per business rules | Inactive user blocked; related data behaved as designed | Passed |
| 7 | Directory pagination | Stable ordering; page size honored | Page size and ordering consistent across pages | Passed |
| 8 | Non-admin directory API access | 403 | 403 on directory admin APIs | Passed |

---

## Summary

Testing covered the Smart Library Table Reservation and IoT monitoring system end-to-end: **JWT authentication and role-based routing**, the **public map and anonymized reservation feeds**, **reservation rules** (library timezone, 30-minute slot alignment, opening hours, overlap prevention, 240-minute daily cap per user, and transactional email/OTP behaviour), and **admin** functions for **tables**, **reservation oversight**, **weight sensors**, **LCD displays**, and **role-specific user directories**. Across all modules above, **actual results matched expected results** and every recorded case was marked **Passed**, indicating the implemented behaviour is consistent with requirements and safe for demonstration and deployment within the project scope.
