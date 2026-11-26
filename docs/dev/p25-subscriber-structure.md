<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# P25 subscriber programming and mobility proposal

This proposal is based on main `900427cb891f5407bf53a819c995dcb002a21eee`.
It describes the proposed behavior and acceptance criteria, not completed support.
The scope is Phase 1 FDMA subscriber programming, same-system site mobility and
talkgroup monitoring. It does not claim complete P25 conformance, add Phase 2,
or add authentication, encryption, emergency or packet services.

## Ownership

| Owner | Stored configuration | Runtime state |
|---|---|---|
| System | WACN, System ID, subscriber identity, transmit policy, talkgroup membership, programmed identifier seeds and acquisition policy | Validated serving identity, learned identifier table, registration context, assigned IDs and accepted affiliation |
| Site | Parent System, RFSS and Site ID, control-channel candidates, optional expected NAC and explicit restriction policy | Confirmed control channel, learned NAC, location-registration outcome and measured quality |
| Control-channel entry | Parent site, frequency or identifier/channel number, optional explicit uplink | Candidate attempt and validation status |
| Talkgroup | System-qualified group ID and name through existing contact/reference owners | No independently selected transmit target |
| Trunked channel personality | System and selected talkgroup references, receive list, optional site restriction and operating options | The user's selected transmit target |
| Zone | Ordered references to channel personalities | Current selector position |
| Receive/scan list | Qualified group/personality references and monitoring policy | Eligible grants and receive priority; not a second affiliation owner |

A personality can appear in several zones. A zone does not own the talkgroup or
duplicate its System, sites, control frequencies or bandplan. Runtime voice
frequencies come from validated grants and the serving System's identifier table.
Two Systems may use the same group number with different names and membership.
Their contacts and learned state must not be resolved by an unqualified number.

Reuse the existing generic System, entry, contact, target-list, channel and zone
interfaces. Add only the membership or typed fields those interfaces cannot
currently express. Do not introduce a parallel P25 codeplug database. Coordinate
contact changes with the existing P25-contact work in upstream PR 844.

### System talkgroup catalog

A System's talkgroup catalog must be editable before any channel personalities
exist. Create each System-qualified contact once, with its numeric ID, name and
System scope. Personalities and receive lists reference those contacts; creating
several personalities or placing one in several zones does not copy the contact.
The catalog must not be reconstructed from the personalities currently in zones.
Two Systems using the same numeric group ID have distinct qualified contacts.

Extend PR 844's air-contact owner with a System scope reference resolved through
the existing System owner. Keep IDs and names in that contact owner, rather than
adding another P25 talkgroup store or repeating WACN/System ID in each contact.
A System's catalog is the view of contacts scoped to it. Refresh derived catalog
and name caches on the existing codeplug-change notification. All target loading,
RX-list filtering, name lookup, import and export must use the same scope rule.

The wire encoding requires coordination with PR 844 before implementation. Its
24-byte records have reserved bytes, but those bytes do not yet define a System
reference or a scope-presence marker. Agree on that encoding, deletion/reuse and
reordering rules, treatment of legacy unscoped and conventional P25 contacts, and
the firmware capability token. Preserve old records and unknown bytes. Older
readers ignore new scope information, so their existing `p25contacts` token alone
cannot promise System-qualified lookup or enforce a contact/System match. Do not
silently write a scoped profile as though an older reader supported it.

### Public PR dependencies and overlap

These are public review snapshots. The implementation must recheck their current
heads before stacking or extracting changes; the first-patch stack is specified
below.

| Public PR / reviewed head | Reusable owner or behavior | Integration work and limits |
|---|---|---|
| [669: control validation](https://github.com/RadioFoundry/RadioFoundry/pull/669), `41a8d43df1f9` | Existing confirmed-identity gate, conflicting RFSS rejection and registration-context invalidation | Required dependency for the first NAC patch. Integrate the public branch instead of writing a second RFSS consistency guard; preserve the NAC-specific rejected-input and retune regressions through the overlap. This does not add location registration. |
| [844: P25 contacts](https://github.com/RadioFoundry/RadioFoundry/pull/844), `a04521d488bf` | Air-contact table, `p25_channel_talk`, RX-list loading, editor conversion and capability warning | Open draft. Its lookup is mode/type/ID qualified, not System qualified. Extend this owner with scope, scoped caches and catalog editing; do not create a competing contact table. |
| [978: receive-only site selection](https://github.com/RadioFoundry/RadioFoundry/pull/978), `5e7266a7154c` | Decode-coverage scoring, fresh RSSI sampling, hysteresis and bounded candidate survey | Explicitly excludes transmit-enabled Systems. Reconcile candidate identity/NAC, pending requests and location registration before applying its survey policy to subscribers. Its numeric thresholds are product choices. |
| [979: receive-only zone group scan](https://github.com/RadioFoundry/RadioFoundry/pull/979), `d3492c7d7c7c` | One follower filtering same-System groups, selected-group priority and scan-hit presentation | Explicitly excludes transmit-enabled Systems and derives groups from zone personalities. Replace its direct DMR-contact reads with PR 844's target reader. Restore the configured RX list when stopping scan: its current `groups = NULL`, `ngroups = 0` would discard PR 844's list. |
| [1094: stored channel scan lists](https://github.com/RadioFoundry/RadioFoundry/pull/1094), `595e45b50244` | Generic named channel-list storage, editing and channel-reference ownership | Its scanner retunes channel personalities, including trunked stops; it is not a same-System grant filter. Reuse list ownership where applicable, but keep trunk group monitoring in the follower. Storage placement and board support remain separate review questions. |

The selected first-patch sequence is a per-site NAC checkpoint followed by
integration of public PR 669 before final CI and publication review. Keep the
NAC-specific rule that a good block on a rejected NAC cannot clear an existing
unverified-input state. The combined patch must pass the identity and NAC
regressions together. This sequence makes no new hardware or full-roaming claim.

PRs 978 and 979 both change the follower's memory budget and lifecycle, so a
combined branch needs a combined resource check and feature-off builds. Their
reported combined-image radio tests do not qualify an independently stacked
subscriber implementation. No dependency above supplies location registration.

## Bandplans

RadioFoundry already stores identifier rows as System entries of class 2, with
the entry group holding the identifier. Keep that owner. A System's identifier
means the same frequency plan across its RFSSs. Different Systems' identifier
numbers are separate namespaces.

Configured rows can seed acquisition. Keep configured rows separate from learned
information, and make normal subscriber operation apply valid identifier updates
from the confirmed System. An update replaces the previous learned definition
for that identifier. Do not silently persist learned rows into the user's file.
Any explicit manual-plan operating mode must be visible and documented.

A same-System site change retains valid learned identifier definitions and other
applicable System parameters. Do not clear the table per site or add an arbitrary
expiry. A valid update from the confirmed System replaces the matching learned
identifier even when the previous value came from another site of that System.
Secondary and adjacent control-channel records have their own replacement and
invalidation rules. An uncoordinated adjacent System must not overwrite the
serving System's identifier namespace. No universal cache lifetime is assumed.

## NAC and serving context

A System NAC may remain a compatibility default or acquisition hint. An optional
site expectation must take effect in the loader, candidate selection and radio
configuration, not only in the editor. Distinguish absent, automatic and fixed
values, including fixed NAC 000.

Existing programmed RFSS/Site ID fields do not become site locks as a side effect
of this work. Preserve their legacy selection behavior. A new optional restriction
must have an explicit presence/policy field and documented compatibility defaults;
only an enabled restriction rejects an otherwise valid same-System site on that
basis. Observed site identity remains part of the confirmed serving context.

Validate a candidate's WACN/System identity and applicable site constraints before
it can authorize subscriber transmissions. Once validated, use the current site's
NAC for that site's control and traffic channels in both directions. Automatic
reception must narrow to the validated NAC; a move to a new candidate must not
retain the old site's transmit authorization or silently reuse its NAC.

Keep one authoritative confirmed context in the existing follower. The radio's
retune/configuration handoff consumes that context coherently. Stored expectations,
observed identity and confirmation status must remain distinguishable in views.
Existing blocked-system, band, receive-only and transmit-admission policies remain.

## Roaming

Site mobility is separate from selecting a talkgroup. A serving site is retained
while it remains usable under the selected policy. Candidate discovery uses
configured, secondary and adjacent control channels; candidate ranking is not
proof of identity or permission to transmit.

Monitor decode quality and actual control-channel loss. RSSI may supplement those
measurements. Hysteresis, preferred sites, minimum dwell and site restrictions are
product choices with documented defaults; they must not be described as universal
TIA timer values. Candidate sampling must respect pending service requests and
the configured adjacent-site scan interval.

After candidate acquisition, confirm the network/site and NAC. An ordinary site
move after successful registration and affiliation on the same System retains
the assigned WUID, WGID, AGID and applicable System parameters, and performs
location registration. It does not unconditionally repeat full registration and
affiliation. A selected-group change performs affiliation. Fresh or forced full
registration and a System change are separate transitions; leaving an isolated
site also has a full-registration requirement.

The existing follower instead starts full registration when its recorded RFSS or
Site ID changes. Subscriber mobility therefore needs protocol work beyond a
candidate ranking policy: LRA and assigned-ID context, location request/response
encoding, acknowledgement waits, forced registration and distinct site-denial and
System-refusal handling. A registration denial can require hunting another site,
while a registration refusal applies to the System. An affiliation refusal
instead identifies a group that is not valid on that System; preserve the service
and reason when reporting or applying it. Registration, location update, affiliation,
denial, refusal, timeout and lack of a usable control channel need observable
states. Retain accepted affiliation across an ordinary location update, subject
to the response and any required full-registration transition.

This proposal covers same-System site mobility. Selecting another configured
System requires its registration context. Automatic visited-network roaming is
not implied without explicit home/serving-identity and policy support.

## Scan behavior

For a receive list on one System, remain on its serving control channel and filter
grants for the selected group and enabled receive groups. Bind the existing
receive-list owner to the follower's group-filter interface. Do not simulate this
by repeatedly restarting the follower for each talkgroup personality.

Receiving a scanned group does not change the selected transmit group and does not
affiliate every member of the list. If a future explicit talkback mode changes the
transmit target, it must perform the required affiliation before requesting voice.
PTT during passive monitoring returns to the selected target and validates access.

Cross-System scanning is a separate scheduling feature: one receiver cannot
continuously monitor several control channels. It requires explicit acquisition,
departure, registration, affiliation, dwell and PTT restoration behavior. Do not
enable it by simply removing the ordinary scanner's trunk-channel exclusion.

## Compatibility and presentation

Existing upstream codeplugs remain readable. Retain existing record IDs and offsets,
append optional fields, default short records and preserve unknown suffixes.
Legacy profiles keep their documented System defaults. Older firmware cannot
provide behavior associated with fields it does not understand; expose that
capability boundary when writing a profile.

Use the upstream editor, radio UI, translations and generic records. Expose actual
behavior: fields stored but not consumed must not be presented as working roaming
or scan controls. Public changes contain no private source, artwork, credentials,
device data or standards PDFs.

## Reviewable implementation groups

1. Programming and serving context: effective site NAC without changing legacy
   site-selection semantics, identifier ownership, serialization and editor/radio
   agreement, stacked with PR 669's existing identity fixes before final CI. This
   is the first bounded foundation, not subscriber roaming.
2. System-qualified catalog: coordinate scope encoding with PR 844, expose catalog
   membership before personality creation, and make references, lookup caches,
   deletion and export agree.
3. Registered site mobility: protocol messages and registration context first,
   then quality policy and candidate surveys adapted from PR 978. Preserve valid
   System parameters and affiliation through ordinary location updates.
4. Same-System group monitoring: PR 844's receive-list wiring, selected transmit
   target and priority behavior adapted from PR 979, configured-list restoration,
   and PTT transitions. Integrate generic stored lists through their existing owner
   when available; cross-System scheduling remains a separate feature.

These groups can be reviewed as cohesive commits or dependent pull requests.
Do not describe a data-model-only patch as implementing subscriber roaming.

## Acceptance tests

- C and JavaScript consume independent old and extended record vectors with fixed
  NAC 000, automatic NAC, distinct site NACs and unknown trailing bytes.
- A two-site, one-System fixture uses different expected/observed NACs and proves
  the selected site reaches follower and radio RX/TX configuration.
- Legacy RFSS/Site ID values do not restrict acquisition. An explicit restriction
  rejects only the sites its documented policy excludes.
- A mismatched System, explicitly excluded site, unvalidated candidate, stale
  retune or failed NAC confirmation cannot authorize an inbound request or voice
  transmission.
- A return to the control channel restores the current site's configuration.
- A rejected band or HAL tune cannot confirm old-frequency input or retain TALK.
  Failed candidates still honor existing dwell/backoff/fallback timing, and a
  later accepted tune can recover without resetting valid System identifiers.
- Identifier updates replace the matching learned row; two Systems with the same
  identifier number remain isolated; same-System mobility preserves valid context.
- Reference editing, deletion and reordering preserve two Systems' distinct groups
  with the same numeric group ID and preserve ordered zone references.
- Create and round-trip two System catalogs before creating personalities. Reuse
  a contact in several personalities and zones without copying its ID/name; scope
  mismatches, deleted/reused System slots and old firmware are handled explicitly.
- Receive-list grants are admitted or rejected correctly while selected transmit
  group and affiliation remain stable. PTT and any explicit target change have
  observable, tested transitions.
- Starting and stopping a zone/list monitoring override restores the configured
  RX list and uses the P25 contact owner rather than interpreting P25 slots as DMR.
- Mobility tests cover valid loss, weak quality, hysteresis boundaries, candidate
  rejection, registration/location outcomes, affiliation and return to the selected
  operating personality. No test obtains its expected protocol bytes from the
  encoder being tested.
- A same-System site move sends the applicable location request and preserves
  accepted affiliation and valid identifiers; a forced full registration follows
  its own sequence. Acknowledgement, denial, refusal and timeout vectors exercise
  distinct outcomes rather than treating every response as a registration retry.
- Affected feature-off builds, complete host tests, editor tests, translations,
  UI flows/gallery, resource checks and the repository CI gate pass. On-radio and
  independent-network qualification are recorded separately from host results.

## Standards checked

These citations identify requirements and do not prescribe a CPS database schema.
TIA-102.AABD-B is the November 2014 edition. Addenda B-1, B-2 and B-3 cover
Location on PTT, User Alias and Remote Emergency; their scopes do not change the
ownership/acquisition clauses cited here. TIA-102.AABC-E is the April 2019 edition;
its applicable addenda were checked for the cited identifier material.

| Behavior | Document, clause | PDF / printed pages |
|---|---|---|
| Identifier consistency within a System | AABD-B 3.1.1 | 23 / 18 |
| NAC construction and validated site's NAC | AABD-B 3.8 | 29 / 24 |
| Candidate records and control-channel confirmation | AABD-B 5.1, 5.3 | 45–46 / 40–41 |
| Extended hunt and control-channel identity | AABD-B 5.4.5 | 49 / 44 |
| Quality monitoring and reasons to leave | AABD-B 5.5–5.6.1 | 49–50 / 44–45 |
| Adjacent-site scan timing and pending requests | AABD-B 3.6.1; Table 17-1 | 28–29, 139 / 23–24, 134 |
| Full versus location registration and responses | AABD-B 6.2, 6.4–6.5.1 | 52–58 / 47–53 |
| Affiliation and replacing an accepted affiliation | AABD-B 6.7–6.7.2 | 61–62 / 56–57 |
| Group scan and priority monitoring | AABD-B 7.1.3–7.1.5 | 68–69 / 63–64 |
| Identifier update and adjacent records | AABD-B 12.3, 12.6 | 115–117 / 110–112 |
| Channel-identifier update fields | AABC-E 2.3.9.1 | 30 / 11 |

Control identifiers are at most 16 at a site. The 150 ms adjacent-scan value in
Table 17-1 is a default, not an invented universal fixed ceiling. RSSI ranking,
hysteresis numbers, zone organization and scan-list UI remain implementation choices.
