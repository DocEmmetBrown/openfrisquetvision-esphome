class FrisquetProgramCard extends HTMLElement {
  static decode(value) {
    if (!/^[0-9a-f]{84}$/i.test(value)) throw new Error("Programme indisponible ou invalide (84 caractères hexadécimaux attendus).");
    const bytes = value.match(/../g).map((byte) => parseInt(byte, 16));
    return Array.from({ length: 7 }, (_, day) =>
      Array.from({ length: 48 }, (_, slot) => Boolean(bytes[((day + 1) % 7) * 6 + (slot >> 3)] & (1 << (slot % 8)))));
  }

  static encode(days) {
    if (days.length !== 7 || days.some((day) => day.length !== 48 || day.some((value) => typeof value !== "boolean"))) {
      throw new Error("Grille invalide.");
    }
    const bytes = new Array(42).fill(0);
    days.forEach((slots, day) => slots.forEach((on, slot) => {
      if (on) bytes[((day + 1) % 7) * 6 + (slot >> 3)] |= 1 << (slot % 8);
    }));
    return bytes.map((byte) => byte.toString(16).padStart(2, "0")).join("").toUpperCase();
  }

  static periods(slots) {
    const periods = [];
    for (let slot = 0; slot < 48; slot++) {
      if (!slots[slot]) continue;
      const start = slot;
      while (slot < 48 && slots[slot]) slot++;
      periods.push({ start, end: slot });
    }
    return periods;
  }

  static slotsFromPeriods(periods) {
    const slots = new Array(48).fill(false);
    for (const { start, end } of periods) {
      if (!Number.isInteger(start) || !Number.isInteger(end) || start < 0 || end > 48 || start >= end) {
        throw new Error("Chaque période doit finir après son début, dans la même journée (00:00–24:00).");
      }
      for (let slot = start; slot < end; slot++) {
        if (slots[slot]) throw new Error("Les périodes de chauffage ne doivent pas se chevaucher.");
        slots[slot] = true;
      }
    }
    return slots;
  }

  static time(slot) {
    return `${String(slot >> 1).padStart(2, "0")}:${slot % 2 ? "30" : "00"}`;
  }

  static validateTemperatures(values) {
    for (const [index, value] of values.entries()) {
      if (!Number.isFinite(value) || value < 5 || value > (index === 2 ? 15 : 30) || !Number.isInteger(value * 2)) {
        throw new Error("Températures : pas de 0,5 °C, entre 5 et 30 °C (15 °C maximum pour Hors-gel).");
      }
    }
    if (values.length !== 3 || values[2] > values[1] || values[1] > values[0]) {
      throw new Error("Respectez Hors-gel ≤ Réduit ≤ Confort.");
    }
  }

  static parseProfile(text) {
    const profile = JSON.parse(text);
    if (!profile || profile.v !== 1 || typeof profile.n !== "string" ||
        !profile.n.trim() || profile.n.length > 40 || typeof profile.p !== "string") {
      throw new Error("Profil invalide ou version non prise en charge.");
    }
    FrisquetProgramCard.decode(profile.p);
    FrisquetProgramCard.validateTemperatures([profile.c, profile.r, profile.f]);
    return profile;
  }

  setConfig(config) {
    if (!config.entity?.startsWith("text.")) throw new Error("entity doit désigner le texte du programme Frisquet.");
    if (config.profile_management !== undefined && typeof config.profile_management !== "boolean") {
      throw new Error("profile_management doit être un booléen.");
    }
    if (config.profile_management && !config.profiles) throw new Error("La gestion des profils nécessite profiles.");
    if (config.profiles !== undefined) {
      if (!Array.isArray(config.profiles) || !config.profiles.length ||
          config.profiles.some((entity) => typeof entity !== "string" || !/^input_text\.[a-z0-9_]+$/.test(entity)) ||
          new Set(config.profiles).size !== config.profiles.length ||
          !/^esphome\.[a-z0-9_]+$/.test(config.profile_action || "") ||
          !["comfort_entity", "reduced_entity", "frost_entity"].every((key) => /^number\.[a-z0-9_]+$/.test(config[key] || ""))) {
        throw new Error("Profils : configurez des input_text distincts, profile_action (esphome.…) et les trois entités number.");
      }
    }
    this.config = config;
    this.days = null;
    this.base = null;
    this.pending = null;
    this.editor = null;
    this.temperatures = null;
    this.baseTemperatures = null;
    this.pendingTemperatures = null;
    this.selectedProfile = config.profiles?.[0];
    this.profileName = "";
    this.profileBusy = false;
    this.message = "";
    this.attachShadowIfNeeded();
    this.build();
    if (this._hass) this.hass = this._hass;
  }

  attachShadowIfNeeded() {
    if (!this.shadowRoot) this.attachShadow({ mode: "open" });
  }

  set hass(hass) {
    this._hass = hass;
    if (!this.config) return;
    const state = hass.states[this.config.entity]?.state;
    const wasClean = !this.dirty && !this.editor;
    this.current = /^[0-9a-f]{84}$/i.test(state || "") ? state.toUpperCase() : null;
    this.currentTemperatures = this.readTemperatures();
    if (this.pending && this.current === this.pending &&
        (!this.pendingTemperatures || this.sameTemperatures(this.pendingTemperatures, this.currentTemperatures))) {
      this.base = this.current;
      this.baseTemperatures = this.currentTemperatures?.slice();
      this.pending = null;
      this.pendingTemperatures = null;
      this.message = "Programme reçu depuis la chaudière.";
    }
    if (this.current && !this.pending && wasClean) {
      this.base = this.current;
      this.days = FrisquetProgramCard.decode(this.current);
      this.baseTemperatures = this.currentTemperatures?.slice();
      this.temperatures = this.currentTemperatures?.slice();
    }
    this.refresh();
  }

  readTemperatures() {
    if (!this.config.profiles) return null;
    const values = ["comfort_entity", "reduced_entity", "frost_entity"].map((key) => {
      const state = this._hass.states[this.config[key]]?.state;
      return state === undefined || state.trim() === "" ? NaN : Number(state);
    });
    return values.every(Number.isFinite) ? values : null;
  }

  sameTemperatures(a, b) {
    return Boolean(a && b && a.length === b.length && a.every((value, index) => value === b[index]));
  }

  get conflict() {
    return Boolean(this.current && this.base && (this.current !== this.base ||
      (this.config.profiles && !this.sameTemperatures(this.currentTemperatures, this.baseTemperatures))));
  }

  get dirty() {
    return Boolean(this.days && (FrisquetProgramCard.encode(this.days) !== this.base ||
      (this.config.profiles && this.temperatures && !this.sameTemperatures(this.temperatures, this.baseTemperatures))));
  }

  build() {
    this.shadowRoot.innerHTML = `
      <style>
        ha-card { padding: 12px; }
        h2 { margin: 0 0 8px; font-size: 18px; font-weight: 500; }
        p { margin: 6px 0; line-height: 1.35; }
        :host { --comfort: #f5a66c; --reduced: #69b4fa; display: block; }
        * { box-sizing: border-box; }
        .legend { display: flex; gap: 12px; flex-wrap: wrap; margin: 8px 0 4px; font-size: 12px; }
        .key { display: inline-block; width: 12px; height: 12px; border-radius: 3px; margin-right: 6px; }
        .comfort { background: var(--comfort); }
        .reduced { background: var(--reduced); }
        .day { display: grid; grid-template-columns: 72px minmax(0, 1fr); column-gap: 8px; width: 100%;
          text-align: left; border: 0; border-bottom: 1px solid var(--divider-color, #ddd);
          border-radius: 0; padding: 8px 0 4px; min-height: 44px; }
        .day-heading { grid-column: 1; grid-row: 1 / 3; align-self: start; font-size: 12px; font-weight: 500; }
        .band { grid-column: 2; display: flex; height: 14px; border-radius: 4px; overflow: hidden; }
        .segment + .segment { border-left: 2px solid var(--card-background-color, white); }
        .axis { grid-column: 2; display: block; position: relative; height: 14px; margin-top: 2px; font-size: 10px;
          color: var(--secondary-text-color, #666); }
        .axis:empty { display: none; }
        .tick { position: absolute; width: 32px; text-align: center; }
        .ranges { display: none; }
        .editor { margin-top: 8px; padding: 8px; border: 1px solid var(--divider-color, #aaa); border-radius: 8px; }
        .editor[hidden] { display: none; }
        h3 { margin: 0 0 8px; }
        .period { display: flex; flex-wrap: wrap; align-items: end; gap: 8px; margin: 8px 0; }
        .period label { display: grid; gap: 4px; }
        .period select { min-width: 90px; }
        fieldset { border: 1px solid var(--divider-color, #aaa); border-radius: 6px; margin: 8px 0 0; }
        .targets { display: flex; gap: 8px; flex-wrap: wrap; padding: 0; }
        .targets label { display: flex; align-items: center; gap: 4px; min-height: 40px; }
        .error { color: var(--error-color, #db4437); }
        button, select, input { font: inherit; color: var(--primary-text-color); background: var(--card-background-color, white);
          border: 1px solid var(--divider-color, #aaa); border-radius: 6px; padding: 8px; }
        button { cursor: pointer; min-height: 40px; }
        button:focus-visible { outline: 3px solid var(--primary-color, #03a9f4); outline-offset: 1px; }
        button:disabled { opacity: .5; cursor: default; }
        .tools { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; margin: 8px 0; }
        .profile-picker { align-items: end; }
        .profile-picker label { flex: 1; min-width: 100px; }
        .profile-picker button { max-width: 50%; }
        .primary { background: var(--primary-color, #03a9f4); color: var(--text-primary-color, white); }
        .status, .write-status { white-space: pre-wrap; overflow-wrap: anywhere; font-size: 12px; }
        .status:empty, .write-status:empty { display: none; }
        details { margin-top: 6px; font-size: 12px; color: var(--secondary-text-color, #666); }
        summary { cursor: pointer; min-height: 32px; display: list-item; align-content: center; }
        .profiles[hidden] { display: none; }
        [hidden] { display: none !important; }
        .profiles label { display: grid; gap: 4px; max-width: 100%; }
        .profiles input, .profiles select { min-width: 0; max-width: 100%; min-height: 40px; }
        .temperatures { display: flex; flex-wrap: wrap; gap: 8px; margin: 8px 0; }
        .temperatures input { width: 90px; }
        .profile-temperatures { font-size: 12px; }
        .profile-management:not([hidden]) + .profile-temperatures { display: none; }
      </style>
      <ha-card>
        <h2>Programme hebdomadaire</h2>
        <section class="profiles" hidden>
          <div class="tools profile-picker">
            <label>Profil enregistré<select class="profile-select"></select></label>
            <button data-action="load-profile" aria-label="Charger le profil">Charger</button>
          </div>
          <div class="profile-management">
            <label>Nom du profil<input class="profile-name" type="text" maxlength="40" placeholder="Travail, Télétravail…"></label>
            <div class="temperatures"></div>
            <div class="tools">
              <button data-action="save-profile" aria-label="Enregistrer le profil dans HA">Enregistrer</button>
              <button data-action="delete-profile" aria-label="Supprimer le profil">Supprimer</button>
            </div>
          </div>
          <p class="profile-temperatures"></p>
        </section>
        <div class="legend">
          <span><span class="key comfort"></span>Chauffage (Confort)</span>
          <span><span class="key reduced"></span>Réduit</span>
        </div>
        <div class="week"></div>
        <section class="editor" hidden aria-label="Édition d'une journée">
          <h3></h3>
          <p>En dehors des périodes ci-dessous, le chauffage est en Réduit. Pour passer minuit, utilisez deux journées.</p>
          <div class="periods"></div>
          <button data-action="add">Ajouter une période</button>
          <p class="error" role="alert"></p>
          <fieldset>
            <legend>Copier cette journée vers…</legend>
            <div class="targets"></div>
          </fieldset>
          <div class="tools">
            <button data-action="save">Valider la journée</button>
            <button data-action="copy">Valider et copier aux jours cochés</button>
            <button data-action="close">Fermer sans valider</button>
          </div>
        </section>
        <div class="tools">
          <button class="primary" data-action="apply">Appliquer le programme</button>
          <button data-action="reset">Annuler les modifications</button>
        </div>
        <p class="status" role="status" aria-live="polite"></p>
        <p class="write-status"></p>
        <details>
          <summary>Aide et fonctionnement</summary>
          <p>Cliquez sur un jour pour modifier ses périodes de chauffage (pas de 30 minutes).</p>
          <p class="profile-help"></p>
          <p>Le programme s'exécute en mode Auto. Cette carte ne change pas le mode de chauffage.
            L'application peut attendre le prochain échange du satellite (jusqu'à 10 minutes).</p>
        </details>
      </ha-card>`;
    this.buildProfiles();
    this.names = ["Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi", "Dimanche"];
    this.dayButtons = this.names.map((name, day) => {
      const button = document.createElement("button");
      button.className = "day";
      button.onclick = () => this.openDay(day);
      this.shadowRoot.querySelector(".week").append(button);
      return button;
    });
    this.shadowRoot.querySelector('[data-action="add"]').onclick = () => {
      const occupied = new Array(48).fill(false);
      for (const period of this.editor.periods) {
        for (let slot = period.start; slot < period.end; slot++) occupied[slot] = true;
      }
      const start = occupied.indexOf(false);
      if (start < 0) {
        this.shadowRoot.querySelector(".error").textContent = "La journée entière est déjà en chauffage.";
        return;
      }
      this.editor.periods.push({ start, end: start + 1 });
      this.renderPeriods();
    };
    this.shadowRoot.querySelector('[data-action="save"]').onclick = () => this.saveDay(false);
    this.shadowRoot.querySelector('[data-action="copy"]').onclick = () => this.saveDay(true);
    this.shadowRoot.querySelector('[data-action="close"]').onclick = () => this.closeDay();
    this.shadowRoot.querySelector('[data-action="apply"]').onclick = () => this.apply();
    this.shadowRoot.querySelector('[data-action="reset"]').onclick = () => {
      if ((this.dirty || this.pending || this.editor) && !window.confirm("Abandonner les modifications locales et reprendre la dernière valeur reçue par HA ?")) return;
      this.pending = null;
      this.pendingTemperatures = null;
      this.base = this.current;
      this.days = this.current ? FrisquetProgramCard.decode(this.current) : null;
      this.baseTemperatures = this.currentTemperatures?.slice();
      this.temperatures = this.currentTemperatures?.slice();
      this.message = "";
      this.editor = null;
      this.shadowRoot.querySelector(".editor").hidden = true;
      this.refresh();
    };
    this.refresh();
  }

  buildProfiles() {
    if (!this.config.profiles) return;
    this.shadowRoot.querySelector(".profiles").hidden = false;
    this.shadowRoot.querySelector(".profile-management").hidden = !this.config.profile_management;
    this.shadowRoot.querySelector(".profile-help").textContent = this.config.profile_management
      ? "Gérez les profils ici. Enregistrer ne modifie pas la chaudière ; activez un profil depuis l'onglet Chauffage."
      : "Chargez un profil, puis appliquez le programme et ses températures. Gérez les profils dans l'onglet Plannings. Le mode actuel est conservé.";
    this.shadowRoot.querySelector('[data-action="apply"]').hidden = Boolean(this.config.profile_management);
    const select = this.shadowRoot.querySelector(".profile-select");
    for (const [index, entity] of this.config.profiles.entries()) {
      const option = document.createElement("option");
      option.value = entity;
      option.textContent = `Emplacement ${index + 1}`;
      select.append(option);
    }
    select.value = this.selectedProfile;
    select.onchange = () => {
      this.selectedProfile = select.value;
      this.profileName = "";
      this.shadowRoot.querySelector(".profile-name").value = "";
      this.message = "Emplacement sélectionné. Cliquez sur Charger pour ouvrir son planning, ou Enregistrer pour y sauvegarder votre brouillon.";
      this.refresh();
    };
    this.shadowRoot.querySelector(".profile-name").oninput = (event) => {
      this.profileName = event.target.value;
    };
    ["Confort", "Réduit", "Hors-gel"].forEach((name, index) => {
      const label = document.createElement("label");
      label.append(document.createTextNode(`${name} (°C)`));
      const input = document.createElement("input");
      input.type = "number";
      input.min = 5;
      input.max = index === 2 ? 15 : 30;
      input.step = 0.5;
      input.setAttribute("aria-label", `Température ${name}`);
      input.oninput = () => {
        if (!this.temperatures) this.temperatures = [NaN, NaN, NaN];
        this.temperatures[index] = input.value === "" ? NaN : Number(input.value);
        this.refresh();
      };
      label.append(input);
      this.shadowRoot.querySelector(".temperatures").append(label);
    });
    this.shadowRoot.querySelector('[data-action="load-profile"]').onclick = () => this.loadProfile();
    this.shadowRoot.querySelector('[data-action="save-profile"]').onclick = () => this.saveProfile();
    this.shadowRoot.querySelector('[data-action="delete-profile"]').onclick = () => this.deleteProfile();
  }

  loadProfile() {
    try {
      const value = this._hass.states[this.selectedProfile]?.state;
      const profile = FrisquetProgramCard.parseProfile(value);
      if ((this.dirty || this.editor) && !window.confirm("Remplacer le brouillon actuel par ce profil ?")) return;
      this.days = FrisquetProgramCard.decode(profile.p);
      this.temperatures = [profile.c, profile.r, profile.f];
      this.profileName = profile.n;
      this.editor = null;
      this.shadowRoot.querySelector(".editor").hidden = true;
      this.shadowRoot.querySelector(".profile-name").value = profile.n;
      this.message = `Profil « ${profile.n} » chargé localement, pas encore appliqué.`;
    } catch (error) {
      this.message = `Impossible de charger le profil : ${error.message || error}`;
    }
    this.refresh();
  }

  async saveProfile() {
    if (!this.config.profile_management || this.profileBusy || this.pending || this.sending || this.editor) return;
    try {
      FrisquetProgramCard.validateTemperatures(this.temperatures || []);
      const profile = {
        v: 1, n: this.profileName.trim(), p: FrisquetProgramCard.encode(this.days),
        c: this.temperatures[0], r: this.temperatures[1], f: this.temperatures[2],
      };
      const value = JSON.stringify(profile);
      FrisquetProgramCard.parseProfile(value);
      const entity = this._hass.states[this.selectedProfile];
      if (!entity || ["unknown", "unavailable"].includes(entity.state)) throw new Error("Emplacement HA indisponible.");
      if (value.length > (entity.attributes?.max ?? 255)) throw new Error("Capacité du helper insuffisante ; configurez max: 255.");
      if (entity.state && !window.confirm("Remplacer le profil enregistré dans cet emplacement ?")) return;
      await this.storeProfile(value);
    } catch (error) {
      this.message = `Enregistrement impossible : ${error.message || error}`;
      this.refresh();
    }
  }

  async deleteProfile() {
    if (!this.config.profile_management || this.profileBusy || this.pending || this.sending) return;
    if (!window.confirm("Supprimer le profil enregistré ? Le programme chaudière reste inchangé.")) return;
    try {
      await this.storeProfile("");
    } catch (error) {
      this.message = `Suppression impossible : ${error.message || error}`;
      this.refresh();
    }
  }

  async storeProfile(value) {
    this.profileBusy = true;
    this.message = "";
    this.refresh();
    try {
      await this._hass.callService("input_text", "set_value", { entity_id: this.selectedProfile, value });
      this.message = "Stockage demandé à HA. Le contenu reçu du helper apparaît dans la liste des profils.";
    } finally {
      this.profileBusy = false;
      this.refresh();
    }
  }

  refreshProfiles(locked) {
    if (!this.config.profiles) return;
    const options = this.shadowRoot.querySelector(".profile-select").options;
    for (const [index, entity] of this.config.profiles.entries()) {
      const value = this._hass?.states[entity]?.state;
      let label;
      if (value === undefined || ["unknown", "unavailable"].includes(value)) label = "Indisponible";
      else if (!value) label = "Vide";
      else {
        try { label = FrisquetProgramCard.parseProfile(value).n; }
        catch (error) { label = `Invalide : ${error.message}`; }
      }
      options[index].textContent = `${index + 1} — ${label}`;
    }
    for (const input of this.shadowRoot.querySelectorAll(".profiles input, .profiles select, .profiles button")) {
      input.disabled = Boolean(locked || this.profileBusy);
    }
    this.shadowRoot.querySelector('[data-action="save-profile"]').disabled = Boolean(locked || this.profileBusy || this.editor);
    const inputs = this.shadowRoot.querySelectorAll(".temperatures input");
    inputs.forEach((input, index) => {
      if (this.shadowRoot.activeElement !== input) {
        const value = this.temperatures?.[index];
        input.value = Number.isFinite(value) ? value : "";
      }
    });
    this.shadowRoot.querySelector('[data-action="apply"]').textContent = "Appliquer le programme et les températures";
    this.shadowRoot.querySelector(".profile-temperatures").textContent = this.temperatures
      ? `Confort : ${this.temperatures[0]} °C · Réduit : ${this.temperatures[1]} °C · Hors-gel : ${this.temperatures[2]} °C`
      : "Consignes indisponibles.";
  }

  openDay(day) {
    if (this.editor && !window.confirm("Abandonner l'édition de la journée en cours ?")) return;
    this.editor = { day, periods: FrisquetProgramCard.periods(this.days[day]) };
    const panel = this.shadowRoot.querySelector(".editor");
    panel.hidden = false;
    panel.querySelector("h3").textContent = this.names[day];
    const targets = panel.querySelector(".targets");
    targets.replaceChildren();
    this.names.forEach((name, index) => {
      if (index === day) return;
      const label = document.createElement("label");
      const checkbox = document.createElement("input");
      checkbox.type = "checkbox";
      checkbox.value = index;
      label.append(checkbox, document.createTextNode(name));
      targets.append(label);
    });
    this.renderPeriods();
    this.refresh();
    panel.querySelector("h3").setAttribute("tabindex", "-1");
    panel.querySelector("h3").focus();
  }

  renderPeriods() {
    const container = this.shadowRoot.querySelector(".periods");
    container.replaceChildren();
    this.shadowRoot.querySelector(".error").textContent = "";
    if (!this.editor.periods.length) {
      const text = document.createElement("p");
      text.textContent = "Toute la journée en Réduit.";
      container.append(text);
    }
    this.editor.periods.forEach((period, index) => {
      const row = document.createElement("div");
      row.className = "period";
      for (const [field, title, first, last] of [["start", "Début", 0, 47], ["end", "Fin", 1, 48]]) {
        const label = document.createElement("label");
        label.append(document.createTextNode(title));
        const select = document.createElement("select");
        select.setAttribute("aria-label", `${title} période ${index + 1}`);
        for (let slot = first; slot <= last; slot++) {
          const option = document.createElement("option");
          option.value = slot;
          option.textContent = FrisquetProgramCard.time(slot);
          select.append(option);
        }
        select.value = period[field];
        select.onchange = () => { period[field] = Number(select.value); };
        label.append(select);
        row.append(label);
      }
      const remove = document.createElement("button");
      remove.textContent = "Supprimer";
      remove.setAttribute("aria-label", `Supprimer la période ${index + 1}`);
      remove.onclick = () => {
        this.editor.periods.splice(index, 1);
        this.renderPeriods();
      };
      row.append(remove);
      container.append(row);
    });
  }

  saveDay(copy) {
    const error = this.shadowRoot.querySelector(".error");
    if (!this.current || (this.conflict && !this.config.profile_management) || this.sending || this.pending) {
      error.textContent = "Programme indisponible ou modifié sur la chaudière. Annulez les modifications avant de recommencer.";
      return;
    }
    let slots;
    try {
      slots = FrisquetProgramCard.slotsFromPeriods(this.editor.periods);
    } catch (reason) {
      error.textContent = reason.message;
      return;
    }
    const targets = copy ? [...this.shadowRoot.querySelectorAll(".targets input:checked")].map((input) => Number(input.value)) : [];
    if (copy && !targets.length) {
      error.textContent = "Cochez au moins un jour destinataire.";
      return;
    }
    if (copy && !window.confirm(`Remplacer les périodes de ${targets.map((day) => this.names[day]).join(", ")} ?`)) return;
    this.days[this.editor.day] = slots;
    for (const day of targets) this.days[day] = [...slots];
    this.message = this.config.profile_management
      ? "Journée validée localement. Enregistrez le profil dans HA pour la conserver."
      : "Journée validée localement. Cliquez sur Appliquer le programme pour l'envoyer.";
    this.closeDay();
  }

  closeDay() {
    this.editor = null;
    this.shadowRoot.querySelector(".editor").hidden = true;
    this.refresh();
  }

  async apply() {
    if (this.config.profile_management || !this.dirty || !this.current || this.pending || this.sending || this.editor || this.conflict || this.profileBusy) return;
    if (this.config.profiles) {
      try { FrisquetProgramCard.validateTemperatures(this.temperatures || []); }
      catch (error) { this.message = error.message; this.refresh(); return; }
    }
    if (!window.confirm(this.config.profiles ? "Envoyer le programme et les trois températures ? Le mode actuel sera conservé." : "Envoyer ce programme à la chaudière ?")) return;
    const value = FrisquetProgramCard.encode(this.days);
    this.sending = true;
    this.pending = value;
    this.pendingTemperatures = this.config.profiles ? this.temperatures.slice() : null;
    this.message = "";
    this.refresh();
    try {
      if (this.config.profiles) {
        const [domain, service] = this.config.profile_action.split(".");
        await this._hass.callService(domain, service, {
          program: value, comfort: this.temperatures[0], reduced: this.temperatures[1], frost: this.temperatures[2],
        });
      } else {
        await this._hass.callService("text", "set_value", { entity_id: this.config.entity, value });
      }
    } catch (error) {
      this.pending = null;
      this.pendingTemperatures = null;
      this.message = `Échec de l'envoi : ${error.message || error}`;
    } finally {
      this.sending = false;
      this.refresh();
    }
  }

  refresh() {
    if (!this.dayButtons) return;
    const locked = !this.days || !this.current || Boolean(this.pending) || this.sending || this.profileBusy;
    this.refreshProfiles(locked);
    this.dayButtons.forEach((button, day) => {
      const slots = this.days?.[day];
      button.replaceChildren();
      const heading = document.createElement("span");
      heading.className = "day-heading";
      heading.textContent = `${this.names[day]}  ›`;
      const band = document.createElement("span");
      band.className = "band";
      band.setAttribute("aria-hidden", "true");
      if (slots) {
        for (let start = 0; start < 48;) {
          let end = start + 1;
          while (end < 48 && slots[end] === slots[start]) end++;
          const segment = document.createElement("span");
          segment.className = `segment ${slots[start] ? "comfort" : "reduced"}`;
          segment.style.width = `${(end - start) / 48 * 100}%`;
          segment.title = `${FrisquetProgramCard.time(start)}–${FrisquetProgramCard.time(end)} : ${slots[start] ? "Chauffage" : "Réduit"}`;
          band.append(segment);
          start = end;
        }
      }
      const axis = document.createElement("span");
      axis.className = "axis";
      // Stagger close transitions across two lines to keep half-hour labels readable on phones.
      let ticks = slots ? slots.flatMap((on, slot) => slot && on !== slots[slot - 1] ? [slot] : []) : [];
      const axisWidth = Math.max(1, button.getBoundingClientRect().width - 80);
      const positions = (values) => {
        const ends = [-Infinity, -Infinity];
        return values.map((slot) => {
          const left = Math.max(0, Math.min(axisWidth - 32, slot / 48 * axisWidth - 16));
          const row = ends.findIndex((end) => left >= end + 3);
          if (row >= 0) ends[row] = left + 32;
          return row;
        });
      };
      let rows = positions(ticks);
      if (rows.includes(-1)) {
        ticks = [12, 24, 36];
        rows = positions(ticks);
      }
      ticks.forEach((slot, index) => {
        const tick = document.createElement("span");
        tick.className = "tick";
        tick.style.left = `clamp(0px, calc(${slot / 48 * 100}% - 16px), calc(100% - 32px))`;
        tick.style.top = rows[index] ? "13px" : "0";
        tick.textContent = FrisquetProgramCard.time(slot);
        axis.append(tick);
      });
      if (rows.includes(1)) axis.style.height = "27px";
      const ranges = document.createElement("span");
      ranges.className = "ranges";
      const periods = slots ? FrisquetProgramCard.periods(slots) : [];
      ranges.textContent = !slots ? "Programme indisponible" : periods.length
        ? `Chauffage : ${periods.map(({ start, end }) => `${FrisquetProgramCard.time(start)}–${FrisquetProgramCard.time(end)}`).join(" · ")}`
        : "Réduit toute la journée";
      button.append(heading, band, axis, ranges);
      button.setAttribute("aria-label", `Modifier ${this.names[day]} : ${ranges.textContent}`);
      button.title = `${this.names[day]} : ${ranges.textContent}`;
      button.setAttribute("aria-expanded", String(this.editor?.day === day));
      button.disabled = locked;
    });
    for (const input of this.shadowRoot.querySelectorAll(".editor button, .editor select, .editor input")) {
      input.disabled = locked;
    }
    const conflict = this.conflict;
    this.shadowRoot.querySelector('[data-action="apply"]').disabled = locked || !this.dirty || conflict || Boolean(this.editor);
    this.shadowRoot.querySelector('[data-action="reset"]').disabled = !this.current || this.sending || this.profileBusy;
    let status;
    if (!this.current) status = `Entité ${this.config.entity} absente, indisponible ou programme invalide. Vérifiez l'identifiant et la synchronisation ESPHome.`;
    else if (this.config.profiles && !this.currentTemperatures) status = "Consignes chaudière indisponibles. Vérifiez comfort_entity, reduced_entity et frost_entity.";
    else if (this.pending) status = "Commande envoyée, en attente du programme relu. Consultez l'état d'écriture ci-dessous ; en cas de refus, rechargez avant de réessayer.";
    else if (conflict && !this.config.profile_management) status = "Le programme ou les consignes ont changé sur la chaudière pendant l'édition. Annulez les modifications pour reprendre la dernière valeur reçue.";
    else if (this.editor) status = "Édition locale : validez la journée, puis appliquez le programme.";
    else status = this.message || (this.dirty ? "Modifications locales non envoyées." : "Programme reçu. Aucune modification locale.");
    this.shadowRoot.querySelector(".status").textContent = status;
    const write = this.config.write_entity ? this._hass?.states[this.config.write_entity]?.state : null;
    this.shadowRoot.querySelector(".write-status").textContent = this.config.write_entity
      ? `État de l'écriture : ${write || `entité ${this.config.write_entity} introuvable`}` : "";
  }

  getCardSize() {
    const height = this.shadowRoot?.querySelector("ha-card")?.getBoundingClientRect().height;
    return height ? Math.ceil(height / 50) : this.editor ? 22 : 10;
  }
}

customElements.define("frisquet-program-card", FrisquetProgramCard);
window.customCards = window.customCards || [];
window.customCards.push({
  type: "frisquet-program-card",
  name: "Programme Frisquet",
  description: "Programme chaudière Confort / Réduit par demi-heure.",
});
