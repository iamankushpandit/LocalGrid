# Open-sourcing LocalGrid

Status: **decided and done**, 2026-09-29 (D76). The repository is public and licensed
**GPL-3.0-or-later**, the recommendation below. This document is kept as the reasoning behind that
choice and the record of what remains open: runtime key provisioning before any binary is offered,
SPDX headers, and the trademark search.

It follows the pattern of the owner's other project, [Braino](https://github.com/iamankushpandit/Gume):
a generated GitHub Pages site, a CI workflow whose single `verify` job is the required check on
protected `main` and `dev` branches, a Pages workflow deploying to the `github-pages` environment,
community files, and issue and pull request templates.

## 1. Decisions needed from the owner

| # | Question | Recommendation |
|---|---|---|
| 1 | Which licence? (section 2) | **GPL-3.0-or-later** for code and docs, the same as Braino |
| 2 | Make the repository public, and when? | After decisions 1 and 3 and the checklist in section 5 |
| 3 | Is the name "LocalGrid" clear to use? (section 5) | Search trademark registers before any promotion |
| 4 | Provision grid keys at runtime instead of compiling them in? (section 3) | Yes, before any binary is offered for download |
| 5 | Accept code contributions before the licence exists? | No. Issues, bench reports, and discussion only |

Record each answer in `docs/DECISIONS.md` as the next D-number.

## 2. Licence options

Everything in the tree is either the owner's own work or third-party material under a licence
compatible with every option below (see [THIRD_PARTY.md](../THIRD_PARTY.md)):

| Material | Licence | Effect on the choice |
|---|---|---|
| Braino-derived board facts and techniques | GPLv3, owner's own copyright | None. The owner can license their own work here under anything. |
| TFT_eSPI panel init sequences | FreeBSD (2-clause BSD) | Keep the notice. Compatible with every option. |
| Montserrat, Font Awesome Free, Noto Emoji | SIL OFL 1.1 | Fonts may be bundled with software under any licence; the fonts keep OFL. |
| ESP-IDF (dependency, not copied) | Apache-2.0 | Compatible with GPLv3, MPL-2.0, Apache-2.0, and MIT. Not with GPLv2-only. |

### The candidates

| Licence | What it asks of others | For LocalGrid | Against |
|---|---|---|---|
| **GPL-3.0-or-later** | Anyone distributing a modified build, or a device running one, offers the full source under the same terms, and (for consumer devices) lets owners install their own build | Matches Braino, so the owner's two projects share one model and material can move between them freely. Handhelds sold as kits stay reflashable and their changes come back. Well understood by hobbyists. | Some companies will not build products on GPL firmware. Linking the core into a closed phone app is not allowed. |
| **MPL-2.0** | Changes to LocalGrid's own files stay open; new files around them may be closed | Kit makers and app developers can build on it while fixes to the protocol core come back | Less familiar. A product can keep its own features closed. Moving code between Braino (GPL) and LocalGrid gets one-directional. |
| **Apache-2.0** | Keep notices; explicit patent grant | Same licence as ESP-IDF; the easiest for companies; patent grant suits a crypto and radio protocol | Anyone can ship a closed fork, and nothing has to come back |
| **MIT** | Keep the notice | Simplest possible | Same as Apache-2.0 but without the patent grant |
| **Split**: core Apache-2.0, firmware GPL-3.0-or-later | Protocol can be implemented anywhere; the device firmware stays copyleft | Lets phone apps or other devices speak the protocol later, while the firmware stays open | Two licences to explain, and a boundary (`components/lg_core`, `lg_crypto`) to keep in every header |

**Recommendation: GPL-3.0-or-later for everything, documents included, with no contributor licence
agreement.** The reasons:

1. It is the owner's existing choice for Braino, which already shares board facts, the battery curve,
   and the lock screen with LocalGrid. One licence means no one-way doors between the projects.
2. The things most worth protecting here are the protocol core and the self-healing behaviour. Under
   GPLv3, a company that ships handhelds must publish its changes and let buyers reflash them. That is
   what an offline network people rely on in an emergency should guarantee.
3. It keeps the relicensing option open: the owner, as copyright holder, can later put
   `lg_core` under a more permissive licence (the split option) if a phone app needs it. Going the
   other way, from permissive to copyleft, cannot recall copies already released. Keep that option by
   asking contributors to agree that their `lg_core` and `lg_crypto` contributions may be relicensed
   under Apache-2.0. One line in CONTRIBUTING.md does it; a CLA does not.

Choose **MPL-2.0** instead if attracting commercial kit makers matters more than guaranteeing their
changes come back.

### When a licence is chosen

- Add `LICENSE` with the unmodified licence text. `tools/gen_site.py` detects it and the site's
  licence line changes by itself.
- Add `NOTICE.md` in Braino's form: copyright holder (the owner as an individual), what the licence
  grants, and a trademark section asking forks that ship to strangers to rename. The GPL does not
  license the name.
- Add SPDX headers (`SPDX-License-Identifier: GPL-3.0-or-later`) to source files, and later a check
  in CI like Braino's `tools/check_licenses.py`.
- Replace the **Licensing** section of CONTRIBUTING.md: contributions under the project licence,
  no CLA, optionally `Signed-off-by` (DCO).
- Update the README's **Licence** section and THIRD_PARTY.md's opening paragraph.

## 3. Why there is no download yet

Braino's site has a flash button, and its release workflow attaches binaries to every tag. LocalGrid
cannot do either yet: **each grid's keys are compiled into the firmware** from
`firmware/common/lg_secrets.h`. A published binary would give everyone who flashes it the same Wi-Fi
passphrase and backbone key, so every "LocalGrid" built from it would be one grid that anyone can
join and read group traffic on.

CI builds with throwaway keys generated on each run and never uploads the images.

**Proposed change (decision 4):** move the grid passphrase and keys out of the build and into NVS at
provisioning time. The device ID already works this way (the `lgid` partition). The first access
point generates the grid's keys at setup on the admin page, and other devices receive them over a
cable or a short pairing step. Once that works:

1. `release.yml` in Braino's form: a `v*` tag builds every firmware and target, refuses a tag with
   no matching `CHANGELOG.md` section, packs merged images with `SHA256SUMS.txt`, and publishes a
   GitHub release with notes lifted from the changelog.
2. A browser installer on the site using [ESP Web Tools](https://esphome.github.io/esp-web-tools/)
   over Web Serial, with images served from the Pages origin, as in Braino.
3. A **Show HN** or equivalent launch. Being able to flash a board from a browser in two minutes is
   what turns interest into users.

Until then the site offers the source, the design documents, and the admin page preview.

## 4. CI/CD, branches, and environments

Prepared in this change:

| File | What it does |
|---|---|
| `.github/workflows/ci.yml` | **plan**: compiles the Python tools, runs the layer check (D27), refuses a tracked secrets file, generates the site, and works out the build matrix from `tools/bench_devices.json`. **build**: one job per firmware and target in the `espressif/idf:v6.1` container, through `tools/build.py --budget 85` (zero warnings, 85% partition budget from design review answer 38). **verify**: the one required check. |
| `.github/workflows/pages.yml` | Generates the site on every push to `main` and deploys it to the `github-pages` environment, but only once the repository is public or the `PAGES_ENABLED` variable is `true`. |
| `tools/gen_site.py`, `site/index.template.html` | The site: every fact read from the tree, plus the real admin page against a simulated MAIN. It refuses to publish COM ports, device IDs, MAC addresses, secret names, or "camp network" wording (D19). |
| README, CONTRIBUTING, SECURITY, CODE_OF_CONDUCT, issue and pull request templates | The community files, adapted from Braino to LocalGrid's rules and definition of done. |

Owner actions, in order (each changes repository settings, so they are not done automatically):

```bash
# 1. A dev branch, the default target for contributions
git push origin main:dev

# 2. Rulesets: pull requests into main and dev, the verify check required, no force pushes
gh api repos/iamankushpandit/LocalGrid/rulesets -X POST --input - <<'JSON'
{"name":"Protect main","target":"branch","enforcement":"active",
 "conditions":{"ref_name":{"include":["refs/heads/main"],"exclude":[]}},
 "rules":[{"type":"deletion"},{"type":"non_fast_forward"},
  {"type":"pull_request","parameters":{"required_approving_review_count":0,"dismiss_stale_reviews_on_push":true,
   "require_code_owner_review":false,"require_last_push_approval":false,"required_review_thread_resolution":false}},
  {"type":"required_status_checks","parameters":{"strict_required_status_checks_policy":false,
   "required_status_checks":[{"context":"verify"}]}}]}
JSON
# (repeat with "Protect dev" and refs/heads/dev)

# 3. Security and community features
gh api repos/iamankushpandit/LocalGrid/private-vulnerability-reporting -X PUT
gh repo edit iamankushpandit/LocalGrid --enable-discussions \
  --description "An offline, self-forming ESP32 network: text, emergencies, and push-to-talk with no Internet" \
  --homepage "https://iamankushpandit.github.io/LocalGrid/" \
  --add-topic esp32,esp-idf,esp-now,mesh-network,offline-first,off-grid,embedded-c,push-to-talk

# 4. Labels the templates use, plus the two newcomers look for
for l in bug board-port proposal "good first issue" "help wanted"; do gh label create "$l" --force; done

# 5. Go public (irreversible in practice: forks and caches keep what they saw)
gh repo edit iamankushpandit/LocalGrid --visibility public --accept-visibility-change-consequences
```

The `github-pages` environment is created by the first deploy. In Settings > Environments, keep its
deployment branch policy at `main` only, as Braino does. Pages source: **GitHub Actions**.

Run the first CI on a pull request before making `verify` required. The `espressif/idf:v6.1`
container and ccache sizes are unmeasured, so the first run confirms the build works headless on
Linux and how long it takes.

## 5. Before going public

| Check | State |
|---|---|
| `lg_secrets.h` or provisioning bundles in git history | **Clean**: only `lg_secrets.example.h` has ever been committed (35 commits checked, 2026-09-18) |
| MAC addresses | **Clean**: the device map holds none by design, and `tools/flash.py` filters them from output |
| `tools/bench_devices.json` | Holds the owner's COM ports and device IDs. Harmless, but contributors would have to edit the owner's map. **Proposed:** commit `bench_devices.example.json` and gitignore the real one, as Braino does with `bench_config.example.json`. |
| Commit author emails | Become public with the history. Use the GitHub `noreply` address for future commits if preferred; existing history keeps what it has. |
| Personal details in `CHANGELOG.md` and milestone reports | Owner first name in one typed example, bench nicknames, a city time zone. Owner to confirm this is fine. |
| Name clearance | "LocalGrid" is short and generic. Before promotion, search the USPTO and EUIPO registers and GitHub for existing uses in radio or networking. |
| `docs/mocks/*.html` load Google Fonts | Fine for design mocks; the published site loads nothing external. |
| Example names | Examples use a group trip that several families join (grid name "Pine Lake Group Trip"), never a single family. |

## 6. Telling people about it

**Positioning.** Today LocalGrid is not a LoRa mesh (LoRa for access points and handhelds is being explored, and the site says so). Meshtastic and its relatives trade bandwidth
for kilometres and usually lean on a phone. LocalGrid uses Wi-Fi and ESP-NOW. It covers a smaller
area and needs powered access points, and in return the handhelds stand alone with no phone, carry
push-to-talk voice, and show who read an emergency. Say this plainly; the comparison will be made
anyway, and honesty about range earns trust.

**Phase 1: ready to be seen (can start once public)**

- Two ad scripts in [ADS.md](ADS.md), question by question, for camping and for a carpool convoy, with a
  claims check and the lines we must not write.
- A 60-second video: two handhelds exchanging messages, an emergency breaking through the lock screen,
  and an access point unplugged with the grid recovering. Put it at the top of the README and the site.
- A social preview image (1280x640) in repository settings, from the brand icon and one screen photo.
- Seed eight to ten **good first issues**: a board profile for a common CYD variant, admin page views
  (message history, a signal map), a theme (D10), docs for one bench procedure, a range test report.
- Write the milestone reports' test procedures as "reproduce this on your bench" guides.

**Phase 2: launch (once a board can be flashed from the browser, section 3)**

| Where | Why |
|---|---|
| Hackaday.io project page, and the Hackaday tip line | The audience that builds this kind of thing; project logs map to milestones |
| r/esp32, the ESP32 forum (esp32.com), Espressif's community showcase | ESP-IDF developers who can port boards |
| Hackster.io | Step-by-step builds reach makers and educators |
| CNX Software tip | Covers ESP32 projects; readers are board-savvy |
| Show HN | The design review and decision log are the story: a network designed in writing before code |
| r/offgrid, r/preppers, outdoor and scouting groups | People who would use it. Lead with the use (a group trip, a festival site, a disaster drill), not the chip. |
| Mastodon and X with #ESP32 | Short clips of the recovery demo |

**Phase 3: keep it moving**

- A short post on the site per milestone, generated from its report.
- A monthly roundup in Discussions: what landed, what boards were ported, what needs testing.
- Credit contributors in `CHANGELOG.md` entries and in the release notes.

## 7. What a contributor can pick up today

| Area | Entry point | Hardware needed |
|---|---|---|
| Admin page | `firmware/node/main/web/admin.html`, previewed with `tools/build_web_preview.py` | None |
| Site and docs | `site/index.template.html`, `tools/gen_site.py`, `docs/` | None |
| Board ports | `components/lg_board` profiles, `lg_bsp` touch and panel drivers | The board |
| Protocol review | `components/lg_core`, `components/lg_crypto`, `tests/target` | One ESP32 to run tests |
| Field testing | `tools/chaos.py`, `tools/serial_capture.py` | Three or more boards |
