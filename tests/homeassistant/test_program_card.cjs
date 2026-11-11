const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const { test } = require("node:test");
const path = require("node:path");

let Card;
const cardWindow = { confirm: () => true };
vm.runInNewContext(fs.readFileSync(path.join(__dirname, "../../homeassistant/frisquet-program-card.js"), "utf8"), {
  HTMLElement: class {},
  customElements: { define: (_, value) => { Card = value; } },
  window: cardWindow,
});

test("card size follows its rendered height instead of reserving unused dashboard space", () => {
  const card = new Card();
  assert.equal(card.getCardSize(), 10);
  card.shadowRoot = { querySelector: () => ({ getBoundingClientRect: () => ({ height: 713 }) }) };
  assert.equal(card.getCardSize(), 15);
  card.shadowRoot = { querySelector: () => ({ getBoundingClientRect: () => ({ height: 1050 }) }) };
  assert.equal(card.getCardSize(), 21);
});

test("Sunday-first bytes and low-bit-first half hours map to Monday-first grid", () => {
  const bytes = new Array(42).fill("00");
  bytes[0] = "01"; // Sunday 00:00
  bytes[6] = "02"; // Monday 00:30
  bytes[41] = "80"; // Saturday 23:30
  const hex = bytes.join("");
  const days = Card.decode(hex);
  assert.equal(days[6][0], true);
  assert.equal(days[0][1], true);
  assert.equal(days[5][47], true);
  assert.equal(days.flat().filter(Boolean).length, 3);
  assert.equal(Card.encode(days), hex);
});

test("all 336 slots round-trip individually", () => {
  for (let byte = 0; byte < 42; byte++) {
    for (let bit = 0; bit < 8; bit++) {
      const bytes = new Array(42).fill("00");
      bytes[byte] = (1 << bit).toString(16).padStart(2, "0");
      const hex = bytes.join("").toUpperCase();
      assert.equal(Card.encode(Card.decode(hex)), hex);
    }
  }
});

test("unavailable and malformed values are rejected, not replaced with an empty schedule", () => {
  for (const value of ["unknown", "unavailable", "", "00", "G".repeat(84)]) {
    assert.throws(() => Card.decode(value));
  }
  assert.throws(() => Card.encode([]));
});

test("period editor handles empty, full day, half hours and adjacent periods", () => {
  const empty = new Array(48).fill(false);
  assert.equal(Card.periods(empty).length, 0);
  assert.equal(Card.periods(new Array(48).fill(true))[0].end, 48);
  const periods = [{ start: 13, end: 16 }, { start: 34, end: 44 }];
  const slots = Card.slotsFromPeriods(periods);
  assert.equal(slots.filter(Boolean).length, 13);
  assert.equal(JSON.stringify(Card.periods(slots)), JSON.stringify(periods));
  assert.equal(Card.time(13), "06:30");
  assert.equal(Card.time(48), "24:00");
  assert.equal(Card.periods(Card.slotsFromPeriods([{ start: 0, end: 1 }, { start: 1, end: 48 }])).length, 1);
});

test("invalid or overlapping periods cannot be silently rounded or applied", () => {
  for (const periods of [
    [{ start: 0, end: 0 }], [{ start: 44, end: 12 }], [{ start: -1, end: 10 }],
    [{ start: 0, end: 49 }], [{ start: 0.5, end: 1 }],
    [{ start: 0, end: 10 }, { start: 9, end: 12 }],
  ]) assert.throws(() => Card.slotsFromPeriods(periods));
});

test("every possible single period round-trips exactly", () => {
  for (let start = 0; start < 48; start++) {
    for (let end = start + 1; end <= 48; end++) {
      const periods = [{ start, end }];
      assert.equal(JSON.stringify(Card.periods(Card.slotsFromPeriods(periods))), JSON.stringify(periods));
    }
  }
});

test("service call uses text.set_value and waits for boiler publication", async () => {
  const card = new Card();
  card.refresh = () => {};
  card.config = { entity: "text.frisquet_zone_1_programme" };
  card.base = card.current = "00".repeat(42);
  card.days = Card.decode(card.base);
  card.days[0][0] = true;
  const expected = Card.encode(card.days);
  let calls = 0;
  card._hass = { callService: async (domain, service, data) => {
    calls++;
    assert.equal(domain, "text");
    assert.equal(service, "set_value");
    assert.equal(data.entity_id, card.config.entity);
    assert.equal(data.value, expected);
  }};
  await card.apply();
  assert.equal(calls, 1);
  assert.equal(card.pending, expected);
  card.hass = { states: { [card.config.entity]: { state: expected } } };
  assert.equal(card.pending, null);
  assert.equal(card.dirty, false);
});

test("service errors are displayed and preserve the draft", async () => {
  const card = new Card();
  card.refresh = () => {};
  card.config = { entity: "text.frisquet_zone_1_programme" };
  card.base = card.current = "00".repeat(42);
  card.days = Card.decode(card.base);
  card.days[0][0] = true;
  card._hass = { callService: async () => { throw new Error("Connection lost"); } };
  await card.apply();
  assert.equal(card.pending, null);
  assert.equal(card.sending, false);
  assert.equal(card.dirty, true);
  assert.match(card.message, /Connection lost/);
});

test("external changes preserve the draft and block writes until reload", async () => {
  const card = new Card();
  card.refresh = () => {};
  card.config = { entity: "text.frisquet_zone_1_programme" };
  card.base = card.current = "00".repeat(42);
  card.days = Card.decode(card.base);
  card.days[0][0] = true;
  card.hass = { states: { [card.config.entity]: { state: "FF".repeat(42) } } };
  assert.equal(card.days[0][0], true);
  assert.equal(card.days[0][1], false);
  assert.notEqual(card.base, card.current);
  await card.apply(); // No callService exists: a write here would fail.
  assert.equal(card.pending, undefined);
});

test("an open day editor retains its baseline on external updates and blocks apply", async () => {
  const card = new Card();
  card.refresh = () => {};
  card.config = { entity: "text.frisquet_zone_1_programme" };
  card.base = card.current = "00".repeat(42);
  card.days = Card.decode(card.base);
  card.editor = { day: 0, periods: [{ start: 12, end: 16 }] };
  card.hass = { states: { [card.config.entity]: { state: "FF".repeat(42) } } };
  assert.equal(card.base, "00".repeat(42));
  assert.equal(card.days[0][0], false);
  card.days[0][0] = true;
  await card.apply();
  assert.equal(card.pending, undefined);
});

test("copying a day changes only selected targets and never sends to HA", () => {
  const card = new Card();
  card.refresh = () => {};
  card.config = { entity: "text.frisquet_zone_1_programme" };
  card.base = card.current = "00".repeat(42);
  card.days = Card.decode(card.base);
  card.names = ["Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi", "Dimanche"];
  card.editor = { day: 0, periods: [{ start: 12, end: 16 }] };
  const error = {};
  card.shadowRoot = {
    querySelector: (selector) => selector === ".error" ? error : {},
    querySelectorAll: () => [{ value: "2" }, { value: "4" }],
  };
  card.saveDay(true);
  for (let day = 0; day < 7; day++) assert.equal(card.days[day][12], [0, 2, 4].includes(day));
  card.days[0][12] = false;
  assert.equal(card.days[2][12], true);
  assert.equal(card.editor, null);
});

test("invalid periods leave the week and editor untouched", () => {
  const card = new Card();
  card.config = { entity: "text.frisquet_zone_1_programme" };
  card.refresh = () => {};
  card.base = card.current = "00".repeat(42);
  card.days = Card.decode(card.base);
  card.editor = { day: 0, periods: [{ start: 20, end: 12 }] };
  const error = {};
  card.shadowRoot = { querySelector: () => error };
  card.saveDay(false);
  assert.match(error.textContent, /finir après/);
  assert.equal(Card.encode(card.days), card.base);
  assert.equal(card.editor.day, 0);
});

function profileCard() {
  const card = new Card();
  card.refresh = () => {};
  card.pending = null;
  card.temperatures = null;
  card.baseTemperatures = null;
  card.config = {
    entity: "text.frisquet_zone_1_programme",
    profiles: ["input_text.frisquet_planning_1", "input_text.frisquet_planning_2"],
    profile_action: "esphome.frisquette_apply_heating_profile",
    comfort_entity: "number.comfort", reduced_entity: "number.reduced", frost_entity: "number.frost",
  };
  card.selectedProfile = card.config.profiles[0];
  card.profileName = "Télétravail";
  card._hass = {
    states: {
      [card.config.entity]: { state: "00".repeat(42) },
      "number.comfort": { state: "20" },
      "number.reduced": { state: "17" },
      "number.frost": { state: "8" },
      "input_text.frisquet_planning_1": { state: "", attributes: { max: 255 } },
      "input_text.frisquet_planning_2": { state: "", attributes: { max: 255 } },
    },
  };
  card.hass = card._hass;
  return card;
}

test("profile storage accepts unicode names and round-trips the exact week and temperatures", async () => {
  const card = profileCard();
  card.config.profile_management = true;
  card.days[0][12] = true;
  card.temperatures = [20.5, 16, 8];
  let saved;
  card._hass.callService = async (domain, service, data) => {
    assert.equal(domain, "input_text");
    assert.equal(service, "set_value");
    assert.equal(data.entity_id, card.selectedProfile);
    saved = data.value;
  };
  await card.saveProfile();
  assert.ok(saved.length <= 255);
  const profile = Card.parseProfile(saved);
  assert.equal(profile.n, "Télétravail");
  assert.equal(profile.c, 20.5);
  assert.equal(profile.r, 16);
  assert.equal(profile.f, 8);
  assert.equal(Card.encode(Card.decode(profile.p)), Card.encode(card.days));
  assert.equal(card.pending, null);
});

test("invalid profiles are never accepted as defaults", () => {
  const valid = { v: 1, n: "Travail", p: "00".repeat(42), c: 20, r: 17, f: 8 };
  for (const patch of [
    { v: 2 }, { n: "" }, { n: "a".repeat(41) }, { p: "00" },
    { c: 20.1 }, { c: 4 }, { r: 21 }, { f: 16 }, { f: 18 }, { c: null },
  ]) assert.throws(() => Card.parseProfile(JSON.stringify({ ...valid, ...patch })));
  assert.throws(() => Card.parseProfile("unavailable"));
  assert.throws(() => Card.parseProfile("null"));
  assert.throws(() => Card.validateTemperatures([]));
});

test("a profile with unchanged program but new temperatures uses one atomic action and waits for all values", async () => {
  const card = profileCard();
  card.temperatures = [21, 18, 9];
  assert.equal(card.dirty, true);
  let calls = 0;
  card._hass.callService = async (domain, service, data) => {
    calls++;
    assert.equal(domain, "esphome");
    assert.equal(service, "frisquette_apply_heating_profile");
    assert.equal(data.program, "00".repeat(42));
    assert.equal(data.comfort, 21);
    assert.equal(data.reduced, 18);
    assert.equal(data.frost, 9);
    assert.equal(data.mode, undefined);
  };
  await card.apply();
  assert.equal(calls, 1);
  card.hass = card._hass; // Old number states must not count as confirmation.
  assert.notEqual(card.pending, null);
  card._hass.states["number.comfort"].state = "21";
  card.hass = card._hass;
  assert.notEqual(card.pending, null);
  card._hass.states["number.reduced"].state = "18";
  card._hass.states["number.frost"].state = "9";
  card.hass = card._hass;
  assert.equal(card.pending, null);
  assert.equal(card.dirty, false);
});

test("setpoint changes during profile editing block activation without altering the draft", async () => {
  const card = profileCard();
  card.temperatures = [21, 17, 8];
  card._hass.states["number.reduced"].state = "16";
  card.hass = card._hass;
  assert.equal(card.conflict, true);
  assert.equal(card.temperatures[0], 21);
  await card.apply(); // No callService: sending would throw.
  assert.equal(card.pending, null);
});

test("storage errors keep the draft, clear the busy flag and report failure", async () => {
  const card = profileCard();
  card.config.profile_management = true;
  card._hass.callService = async () => { throw new Error("Permission denied"); };
  await card.saveProfile();
  assert.match(card.message, /Permission denied/);
  assert.equal(card.profileBusy, false);
  assert.equal(card.profileName, "Télétravail");
});

test("helper capacity and invalid temperatures block saves before calling HA", async () => {
  const card = profileCard();
  card.config.profile_management = true;
  card._hass.states[card.selectedProfile].attributes.max = 100;
  await card.saveProfile();
  assert.match(card.message, /max: 255/);
  card.temperatures = [20.1, 17, 8];
  await card.saveProfile();
  assert.match(card.message, /0,5/);
});

test("a profile saved in HA can be loaded by another card without a boiler write", () => {
  const card = profileCard();
  const profile = { v: 1, n: "Week-end", p: "FF".repeat(42), c: 21, r: 18, f: 8 };
  card._hass.states[card.selectedProfile].state = JSON.stringify(profile);
  const fields = {};
  card.shadowRoot = { querySelector: (selector) => fields[selector] ||= {} };
  card.loadProfile();
  assert.equal(card.profileName, "Week-end");
  assert.equal(Card.encode(card.days), profile.p);
  assert.equal(card.temperatures[0], 21);
  assert.equal(card.dirty, true);
  assert.equal(card.pending, null);
});

test("the management tab cannot send a profile to the boiler", async () => {
  const card = profileCard();
  card.config.profile_management = true;
  card.temperatures = [21, 17, 8];
  await card.apply(); // No callService: a write would fail.
  assert.equal(card.pending, null);
});

test("the heating tab cannot save or delete profiles", async () => {
  const card = profileCard();
  await card.saveProfile();
  await card.deleteProfile();
  assert.equal(card.profileBusy, undefined);
});

test("closing the day editor does not modify the week", () => {
  const card = new Card();
  card.refresh = () => {};
  card.days = Card.decode("00".repeat(42));
  card.editor = { day: 0, periods: [{ start: 12, end: 16 }] };
  const panel = {};
  card.shadowRoot = { querySelector: () => panel };
  card.closeDay();
  assert.equal(card.editor, null);
  assert.equal(panel.hidden, true);
  assert.equal(Card.encode(card.days), "00".repeat(42));
});
