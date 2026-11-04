# Logic review implementation

All six reported bugs and four improvement areas have source changes. No app, browser, build, benchmark, or test suite was run. Go formatting and manual source/diff review are the validation performed in this pass.

| Review item | Implementation |
| --- | --- |
| Literal text/select values reparsed as JSON | API values remain typed values throughout rendering and editing. Only a user-entered progress default is parsed from JSON text. |
| Archived references lost on unrelated edits | Existing unavailable targets remain selected and labeled as preserved. The backend accepts existing inactive references while rejecting newly added ones. Users can still explicitly remove a saved reference. |
| Concurrent old-password login after password change | Session creation checks the verified password hash inside the transaction that inserts the session. A changed hash rejects that login. A password change after session insertion revokes the newly created session. |
| Duplicate Save submissions | The form has a pending-submission guard and disables the actual associated submit button using the submit event's submitter. The guard clears on success or failure. |
| Partial object edits | A single object PATCH accepts metadata and values and commits them together after validation. The original values-only endpoint remains available for checklist toggles. |
| Clearing defaults had no effect | The editor sends `default_value: null` for a cleared default. New object forms also initialize from configured defaults. |
| Repeated database reads | Object pages batch values, relationships, and placements, and reuse definitions per type. Goal evaluation reuses evaluated objects and fields within one read request. |
| No complete object library | Objects tab with search, active type filters, and pages of 50 objects; overview links to the library. Objects do not require a section placement to be browsed. |
| Schema edits invalidated saved values | Field-option edits validate saved values, defaults, and goal targets, including archived data. Select choices support explicit replacement maps. Narrowing allowed reference types requires explicit authorization to remove incompatible links; dependent formulas/progress views block incompatible changes. |
| Files removed before metadata commit | Migration 003 tracks image references by stable asset ID. Deletion commits its intent first and then attempts file cleanup; failures remain queued and can be retried. |

## Browser checks for the user

1. Create text values `null`, `true`, `1e3`, and `[]`, and select choices named `true` and `null`. Edit another property and save; the original strings and selections should remain unchanged.
2. Create a parent-to-child reference, archive the child, edit the parent's name, and restore the child. The relationship should remain. Removing its preserved option explicitly should remove the relationship.
3. Double-click Save when creating a game, object, or goal. Only one record should be created. A rejected save should leave the form editable for a retry.
4. Change an object's name while entering an invalid progress maximum. After the rejected save, reopen the object and verify that neither the name nor values changed.
5. Clear an existing field default, save, and reopen the field. The default should stay cleared. A newly created object should initialize any remaining defaults.
6. Create more than six objects without section placements. Browse them in Objects, filter by type, search by description/name, and verify pagination after 50 objects.
7. Remove a select choice used by an archived object or goal. Saving without a replacement should be blocked; selecting a replacement should update values, defaults, and rule targets together.
8. Narrow a reference field's allowed type. Existing incompatible references should block saving until explicit removal is selected. Dependent relationship formulas/progress views must be adjusted first.
9. Delete an unused image in settings. Referenced images, including archived references and legacy absolute upload URLs, should be protected. Pending cleanup entries should offer a retry and should not be usable as images.

## Prepared automated coverage

`internal/httpapi/review_fixes_test.go` adds regression cases for atomic object saves, archived-reference preservation, stale verified credentials, choice/default/rule migrations, explicit reference cleanup, bulk-versus-individual object data, and durable image cleanup after a filesystem failure. The migration-version expectation now includes migration 003. These tests remain unrun.
