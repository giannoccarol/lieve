# Lieve

<img src="data/lieve.svg" alt="" width="96" align="right">

*A lightweight, native code editor for KDE Plasma (Qt 6 + KDE Frameworks, no Electron), with the
look and the main keyboard shortcuts of VS Code. The documentation is in Italian.*

Editor di codice leggero per KDE Plasma, nativo (Qt 6 + KDE Frameworks, niente Electron).
Fa poche cose, bene: **Esplora risorse**, **Cerca nei file**, **apertura rapida** (`Ctrl+P`),
**editor** e **terminale**, con più progetti aperti insieme (una finestra per progetto),
**Prettier al salvataggio** e un **pannello Git** come in VS Code.
Le icone dei file e le scorciatoie principali sono quelle di VS Code.

Quanto pesa, misurato con lo stesso progetto Angular di medie dimensioni aperto in entrambi:

| | VS Code (con le sue estensioni) | Lieve |
| --- | --- | --- |
| Memoria (PSS) | ~1.270 MB | ~110 MB |
| Processi / thread | 24 / 434 | 2 / 14 |
| Spazio su disco | ~2,7 GB | ~1,4 MB (più le librerie KDE già presenti in Plasma) |

Si avvia in circa un quarto di secondo e ogni progetto aperto in più costa pochi MB.
Non ha language server (niente completamento TypeScript né "vai alla definizione"):
è il prezzo della leggerezza.

## Come è fatto

| Parte | Componente |
| --- | --- |
| Editor | KTextEditor (lo stesso di Kate): evidenziazione per centinaia di linguaggi, multi-cursore, cerca/sostituisci nel file, `.editorconfig` |
| Terminale | Konsole incorporato (konsolepart): usa la tua shell e il tuo profilo Konsole |
| Ricerca e `Ctrl+P` | ripgrep (`rg`): rispetta `.gitignore`, esclude `node_modules` |
| Icone dei file | lette dalle estensioni VS Code già installate (`workbench.iconTheme`, es. vscode-icons) |
| Tema dell'editor | "VSCodium Dark" con il tema scuro di sistema, "Breeze Light" con quello chiaro |
| Aspetto della finestra | colori ricavati dal tema dell'editor (palette Qt + foglio di stile), come in VS Code |
| Più finestre | un solo processo (KDBusService): un secondo `lieve` passa la richiesta a quello aperto |
| Formattazione | il Prettier del progetto (`node_modules`), tenuto caricato da un piccolo processo Node |
| Git | il comando `git` (stato con `--porcelain=v2`), un'operazione alla volta in background |

Il codice è in `src/` (circa 9.700 righe di C++, intestazioni e righe vuote incluse):
`mainwindow` (finestra, schede, sessione del progetto), `windowmanager` (finestre e istanza unica),
`projects` e `projectlist` (elenco e scelta dei progetti), `jumplist` (menu della barra delle applicazioni),
`explorer`, `searchpanel`, `quickopen`, `terminalpanel`, `vscodeicons` (icone e linguaggi di VS Code),
`prettier` (formattazione), `git` e `gitpanel` (controllo del codice sorgente), `theme` (aspetto, riquadri),
`titlebar` (barra del titolo), `statusbar` (barra di stato), `tabview` (schede scorrevoli), `fuzzy`, `settings`.

## Installare

Serve KDE Plasma 6 (o almeno KDE Frameworks 6) con `qt6-base`, `qt6-svg`, `ktexteditor`, `kparts`,
`kdbusaddons`, `kwindowsystem`, `syntax-highlighting`, `konsole` e `ripgrep`, più `git`, `gcc`, `make` e
`pkgconf` per compilare. Su Arch/CachyOS con Plasma quasi tutto è già installato. Non serve CMake.

Il modo più semplice: lo script scarica l'ultima release, la compila e la installa in `~/.local`
(senza `sudo`). Se manca qualcosa dice quali pacchetti installare (Arch e Fedora).

```bash
curl -fsSL https://raw.githubusercontent.com/giannoccarol/lieve/main/install.sh | bash
```

Oppure a mano:

```bash
git clone https://github.com/giannoccarol/lieve.git
cd lieve
make -j$(nproc)
make install        # in ~/.local: bin/lieve, voce nel menu, icona
```

Disinstallare: `make uninstall`.

### Aggiornamenti

Lieve controlla da solo, poco dopo l'avvio e poi ogni 6 ore, se su GitHub è uscita una versione nuova.
Se c'è, nella barra di stato compare **⬆ Aggiornamento**: un clic mostra le novità e **Aggiorna ora**
scarica, compila e installa la nuova versione in background (nella stessa cartella in cui è installato);
alla fine basta **Riavvia**. I sorgenti scaricati restano in `~/.local/share/lieve/source`.
Il controllo si disattiva con `[updates] check=false` nelle impostazioni (`Ctrl+,`).

## Uso

```bash
lieve                      # schermata di scelta del progetto
lieve ~/Progetti/mio-sito
lieve src/app/app.component.ts:42   # apre il progetto (cartella con .git) e va alla riga 42
lieve --open-folder        # chiede quale cartella aprire
```

### Progetti e finestre

- **All'avvio** si sceglie il progetto: prima i *fissati*, poi i *recenti*, poi tutte le cartelle
  trovate nelle cartelle dei progetti (rilevate da sole, es. `~/Progetti` o `~/Documenti/Projects`;
  una cartella "gruppo" che contiene altri repository mostra direttamente quelli). Si filtra scrivendo; `Invio` apre
  quello usato per ultimo, già selezionato.
- **Una finestra per progetto**, tutte nello stesso processo. Ogni progetto ricorda per conto suo
  schede, posizione del cursore, barra laterale, terminale e dimensione della finestra.
- **`Ctrl+R`** apre il selettore di progetti da qualsiasi finestra: `Invio` apre il progetto nella sua
  finestra (o la porta in primo piano se è già aperto), `Ctrl+Invio` lo sostituisce nella finestra
  corrente. Preseleziona il progetto precedente: `Ctrl+R` `Invio` passa avanti e indietro.
- Rilanciare `lieve` (dal menu, da un terminale, anche quello integrato) non avvia un secondo programma:
  apre o porta in primo piano la finestra giusta.
- Clic destro su un progetto: **Fissa in alto**, **Rimuovi dai recenti**, copia percorso, mostra nel file manager.

### Prettier al salvataggio

Segue le stesse regole di VS Code, lette dalle tue impostazioni: `editor.formatOnSave` e
`prettier.requireConfig` dal `.vscode/settings.json` del progetto o dalle impostazioni utente di VS Code,
più `.prettierrc` (con i suoi plugin), `.prettierignore` ed `.editorconfig`.

- Usa il Prettier installato nel progetto (`node_modules/prettier`): progetti senza Prettier non vengono toccati.
- La formattazione avviene subito prima della scrittura: il file viene scritto una volta sola
  (`ng serve` ricompila una volta). Il cursore resta al suo posto e un solo `Ctrl+Z` la annulla.
- Un piccolo processo Node tiene Prettier caricato: parte quando inizi a modificare un file, si chiude
  dopo 10 minuti di inattività. Salvare richiede circa 10-50 ms (il primissimo circa mezzo secondo).
- Se il file ha un errore di sintassi viene salvato così com'è e compare un avviso in basso nell'editor.
- Node viene cercato nel PATH e nei gestori di versione (mise, volta, nvm, fnm), anche quando Lieve
  parte dal menu; si può indicare a mano in `[format] node=`.

### Controllo del codice sorgente (Git)

`Ctrl+Shift+G` o l'icona del ramo nella barra laterale, che mostra il numero di modifiche.

- Sezioni **Modifiche preparate per il commit** e **Modifiche** (più **Conflitti di unione** se ci sono),
  con le icone dei file e le lettere di VS Code: M modificato, A aggiunto, D eliminato (barrato),
  R rinominato, U nuovo, ! conflitto.
- Passando col mouse su un file o su una sezione compaiono i pulsanti: **+** prepara, **−** rimuovi dalla
  preparazione, **annulla modifiche** (chiede conferma; i file nuovi finiscono nel cestino), **apri il file**.
- **Clic** su un file: le differenze in una scheda a sola lettura, righe aggiunte in verde e rimosse in rosso.
  **Doppio clic**: apre il file.
- Messaggio e **Commit** (`Ctrl+Invio`). Dal menu accanto: modifica l'ultimo commit, commit e push,
  commit e sincronizza. Se nulla è preparato, con `git.enableSmartCommit` (come nelle tue impostazioni VS Code)
  il commit include tutte le modifiche; altrimenti lo chiede.
- Quando non ci sono modifiche il pulsante diventa **Sincronizza modifiche ↓ ↑** (pull e push) oppure
  **Pubblica branch** se il branch non è ancora sul server. Dal menu ⋮: pull, push, recupera, prepara/rimuovi
  tutto, **annulla l'ultimo commit** (il messaggio torna nella casella).
- Si aggiorna da solo: al salvataggio, quando cambia `.git` (anche per comandi dati nel terminale), quando torni
  sulla finestra e ogni 10 secondi. Con `git.autofetch` attivo recupera dal server ogni 3 minuti, senza mai
  chiedere password in background.
- Gli hook di git (husky, lint-staged…) trovano `node` anche quando Lieve parte dal menu.
- **Blame in linea**, come GitLens: alla fine della riga del cursore compare in grigio chi l'ha cambiata,
  quando e il messaggio del commit ("Mario Rossi, 1 anno fa • fix: …"); le righe non ancora committate
  dicono "Tu, adesso". Si disattiva con `[git] inlineBlame=false`.

### Aspetto

Tutta la finestra prende i colori dal tema dell'editor, come fa VS Code. Barra laterale, editor e terminale
sono **riquadri con gli angoli arrotondati**, separati da piccoli spazi su uno sfondo più scuro; le schede di
file e terminale sono **a pillola** e la fila delle schede **scorre** con la rotella (niente frecce).

La **barra del titolo** è quella di Lieve: menu a sinistra, al centro una casella con il nome del progetto
che apre `Ctrl+P`, a destra barra laterale / terminale e i pulsanti della finestra in stile Breeze (cerchio
al passaggio del mouse, rosso su chiudi). Si trascina dalla barra, doppio clic ingrandisce, i bordi
(gli spazi attorno ai riquadri) ridimensionano. `[ui] titleBar=system` torna alla barra di Plasma.

In basso c'è la **barra di stato**: a sinistra branch (clic: pannello Git) e ↓↑ da sincronizzare
(clic: sincronizza); a destra chi ha scritto la riga e quando ("Tu, 6 mesi fa", come GitLens, calcolato sul
testo nell'editor), riga e colonna (clic: vai alla riga), spazi/tab (clic: cambia), codifica, fine riga e
linguaggio (clic: i rispettivi menu), ✓ Prettier (clic: formatta ora). La × delle schede compare sulla scheda
attiva e al passaggio del mouse.

Il resto: barre laterali e schede un po' più
scure dell'editor, bordi sottili, barre di scorrimento sottili senza binario, un solo colore d'accento
(scheda attiva, pulsante Commit, campi selezionati, contatori); la voce attiva della barra a sinistra sta su una piastrella arrotondata. Funziona anche con i temi chiari.

- Il colore d'accento si sceglie in `[ui] accent=`: `system` (quello di Plasma), `lieve` (il bordeaux
  dell'icona) o un colore `#rrggbb`.
- `[ui] theme=system` torna ai colori di Plasma per la finestra.
- Il terminale usa gli stessi colori tramite un profilo Konsole privato di Lieve
  (`~/.local/share/lieve/xdg/konsole/`): l'app Konsole e i tuoi profili non vengono toccati.
  Si disattiva con `[terminal] matchEditor=false`.

### Menu della barra delle applicazioni

Con il tasto destro sull'icona di Lieve (barra delle applicazioni o menu Avvio) compaiono i progetti
fissati e recenti (fino a 8), **Scegli progetto…** e **Apri cartella…**. Lieve aggiorna da solo
queste voci in `~/.local/share/applications/lieve.desktop` (solo se è quello installato da
`make install`, riconoscibile dalla riga `X-Lieve-JumpList=true`).

### Scorciatoie

| Azione | Tasti |
| --- | --- |
| Apri file rapidamente (`nome:riga` per andare alla riga) | `Ctrl+P` |
| Cerca nei file (usa il testo selezionato) | `Ctrl+Shift+F` |
| Esplora risorse | `Ctrl+Shift+E` |
| Mostra/nascondi barra laterale | `Ctrl+B` |
| Mostra/nascondi terminale | `Ctrl+J` |
| Vai al terminale / torna all'editor | `` Ctrl+` `` oppure `Ctrl+ò` |
| Nuovo terminale | `` Ctrl+Shift+` `` o il pulsante `+` |
| Cambia progetto | `Ctrl+R` |
| Controllo del codice sorgente (Git) | `Ctrl+Shift+G` |
| Commit (dalla casella del messaggio) | `Ctrl+Invio` |
| Apri cartella | `Ctrl+K` poi `Ctrl+O` |
| Nuova finestra (scelta progetto) | `Ctrl+Shift+N` |
| Chiudi progetto (torna alla scelta) | `Ctrl+K` poi `F` |
| Chiudi finestra / esci da tutto | `Ctrl+Shift+W` / `Ctrl+Q` |
| Salva / Salva tutto | `Ctrl+S` / `Ctrl+K` poi `S` |
| Formatta documento (Prettier) | `Ctrl+Shift+I` oppure `Shift+Alt+F` |
| Salva senza formattare | `Ctrl+K` poi `Ctrl+Shift+S` |
| Chiudi scheda | `Ctrl+W` (o clic centrale sulla scheda) |
| Scheda successiva / precedente | `Ctrl+Tab` / `Ctrl+Shift+Tab` |
| Cerca / sostituisci nel file | `Ctrl+F` / `Ctrl+H` |
| Vai alla riga | `Ctrl+G` |
| Seleziona occorrenza successiva / tutte | `Ctrl+D` / `Ctrl+Shift+L` |
| Sposta / duplica righe | `Alt+↑↓` / `Alt+Shift+↑↓` |
| Elimina riga | `Ctrl+Shift+K` |
| Commenta | `Ctrl+/` |
| Impostazioni | `Ctrl+,` |

Nel terminale restano a Lieve solo `Ctrl+P`, `Ctrl+J`, `Ctrl+Shift+E/F` e `` Ctrl+` ``:
tutti gli altri tasti vanno alla shell (anche `Ctrl+R`, la ricerca nella cronologia).

In Esplora risorse: clic per aprire, `F2` rinomina, `Canc` sposta nel cestino,
clic destro per nuovo file/cartella, copia percorso, apri nel terminale.

## Impostazioni

`Ctrl+,` apre `~/.config/lieve/lieve.conf` (commentato). Si possono cambiare tema dell'editor,
font, minimappa, a capo automatico, scorciatoie VS Code, esclusioni di ricerca ed Esplora risorse,
tema di icone, aspetto (`[ui]`, `[terminal]`), cartelle dei progetti (`[projects] roots=`) e Prettier (`[format]`: `onSave=auto|true|false`,
minuti di attesa prima di chiudere il processo, percorso di node) e Git (`[git]`: `smartCommit`, `autofetch`,
`autofetchMinutes`). Le modifiche valgono dal riavvio.

Progetti recenti e fissati, e la sessione di ogni progetto, sono in `~/.local/state/lieve/state.ini`.

## Pubblicare una nuova versione

Le versioni salgono di uno alla volta: 0.1.0, 0.1.1, 0.1.2, …

1. Aggiorna il numero in `src/version.h`.
2. Aggiungi in cima a `CHANGELOG.md` una sezione `## v0.1.x` con le novità.
3. Fai il commit, poi crea il tag e mandalo su GitHub:

```bash
git tag v0.1.1
git push origin main v0.1.1
```

GitHub Actions compila Lieve su Arch Linux, controlla che il tag corrisponda a `src/version.h` e crea
la release con le note prese da `CHANGELOG.md`. Chi ha Lieve installato la vede comparire nella barra di stato.

## Licenza

Lieve è software libero, distribuito con la GNU General Public License versione 3 (vedi [LICENSE](LICENSE)).
Copyright © 2026 Lorenzo Giannoccaro.

## Crediti

Lieve assembla componenti scritti da altri; il merito del lavoro pesante è loro.

- **KDE Community**: [KTextEditor](https://invent.kde.org/frameworks/ktexteditor) e
  [KSyntaxHighlighting](https://invent.kde.org/frameworks/syntax-highlighting) (editor ed evidenziazione,
  compreso il tema "VSCodium Dark"), [Konsole](https://invent.kde.org/utilities/konsole) (terminale),
  [KParts](https://invent.kde.org/frameworks/kparts), KXmlGui, KCoreAddons, KDBusAddons (istanza unica),
  KWindowSystem (attivazione delle finestre su Wayland). Licenze LGPL/GPL.
- **The Qt Company e contributori**: [Qt 6](https://www.qt.io/).
- **Andrew Gallant (BurntSushi)**: [ripgrep](https://github.com/BurntSushi/ripgrep), usato per ricerca e `Ctrl+P`.
- **vscode-icons team**: [vscode-icons](https://github.com/vscode-icons/vscode-icons) (codice MIT, icone CC BY-SA).
  Le icone non sono incluse in Lieve: vengono lette dall'estensione installata in VS Code.
  Vale lo stesso per [Material Icon Theme](https://github.com/material-extensions/vscode-material-icon-theme)
  di Philipp Kief, se è il tema scelto.
- **Git** (Linus Torvalds, Junio C Hamano e contributori, licenza GPLv2): Lieve usa il comando `git` installato.
- **Prettier** (James Long e contributori, licenza MIT) e **Node.js**: Lieve usa il Prettier già installato
  nel progetto, non ne include una copia.
- **Microsoft**: le definizioni dei linguaggi (estensioni dei file) sono lette dalle estensioni
  integrate di [Visual Studio Code](https://github.com/microsoft/vscode) installato sul sistema.
- Layout e scorciatoie si ispirano a Visual Studio Code.
