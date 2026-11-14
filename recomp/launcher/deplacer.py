#!/usr/bin/env python3
"""Déplacement de l'installation de SpaceAnimalsRecomp dans un autre dossier (issue GitHub n° 14 : « select where the
recomp will live »). Lancé par le launcher (page Réglages, « Emplacement du jeu »), avec le Python fourni.

  python deplacer.py --verifier --racine R --vers P      contrôles rapides, rien n'est déplacé : « @destination D » et
                                                         « @volume meme|autre », ou « @erreur … »
  python deplacer.py --copier --racine R --vers D        launcher ouvert, protocole « @… » : taille, place libre, puis
                                                         copie vers un autre volume (rien à copier sur le même)
  python deplacer.py --finir --pid N --racine R --vers D après la fermeture du launcher (processus N) : même volume, le
                                                         dossier est renommé ; autre volume, l'ancien est supprimé ;
                                                         puis le launcher est relancé (au nouvel endroit, ou à
                                                         l'ancien si rien n'a pu être fait)
  python deplacer.py --abandonner --vers D               copie interrompue (annulée, échec, launcher fermé) : retire le
                                                         dossier commencé, seulement s'il porte la marque de ce script

P est le dossier choisi par le joueur : s'il est vide ou n'existe pas, l'installation y va ; sinon elle va dans
P\\SpaceAnimalsRecomp (refus si ce dossier existe et n'est pas vide).

Même volume : un simple renommage du dossier, fait par PowerShell une fois ce Python terminé (il est dans le dossier à
déplacer, et Windows ne renomme pas un dossier dont un fichier est ouvert). Tout suit, la copie du jeu aussi si elle
est dans le dossier ; le launcher corrige son chemin au démarrage (lanceur.ini, clés dossier et copie).

Autre volume : tout est copié (dates gardées : les étapes de construction déjà faites le restent), contrôlé, puis
l'ancien dossier est supprimé par le Python du nouveau dossier. La copie du jeu du joueur (.nup, .nsp, .xci, .nsz)
n'est ni copiée ni supprimée : elle reste où elle est, et le launcher continue de la lire là.

Rien n'est supprimé tant que la copie n'est pas complète : une copie interrompue laisse l'installation d'origine
intacte. Refus dans un dossier de sources (dépôt cloné, dossier de développement), comme desinstaller.py.
Journal de --finir et --abandonner : %TEMP%\\SpaceAnimalsRecomp-move.log (en anglais).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

LANCEUR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, LANCEUR)
import desinstaller  # noqa: E402  (installation, copies_du_jeu, supprimer, en_marche, attendre, long, go)

NOM_DOSSIER = "SpaceAnimalsRecomp"
EN_COURS = ".deplacement_en_cours"   # marque du dossier de destination pendant la copie (contient l'origine)
PRET = ".deplacement_pret"           # copie complète et contrôlée : --finir peut supprimer l'origine
JOURNAL = os.path.join(tempfile.gettempdir(), "SpaceAnimalsRecomp-move.log")
BLOC = 8 << 20
long = desinstaller.long
go = desinstaller.go


class Echec(Exception):
    pass


def dire(ligne):
    print(ligne, flush=True)


def note(texte):
    try:
        with open(JOURNAL, "a", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S  ") + texte + "\n")
    except OSError:
        pass


def norme(p):
    return os.path.normcase(os.path.abspath(p)).rstrip("\\")


def dedans(p, dossier):
    """p est-il dans dossier (ou dossier lui-même) ?"""
    p, dossier = norme(p), norme(dossier)
    return p == dossier or p.startswith(dossier + os.sep)


def existant(p):
    """Premier dossier existant en remontant depuis p."""
    p = os.path.abspath(p)
    while not os.path.isdir(p):
        haut = os.path.dirname(p)
        if haut == p:
            break
        p = haut
    return p


def meme_volume(a, b):
    """a et b (dossiers, existants ou non) sont-ils sur le même volume ? Dans le doute non : la copie marche partout,
    le renommage seulement sur un même volume. DEPLACER_FORCER_COPIE=1 : toujours non (vérification de la copie sur
    un PC à un seul disque)."""
    if os.environ.get("DEPLACER_FORCER_COPIE") == "1":
        return False
    a, b = existant(a), existant(b)
    try:
        return (os.path.splitdrive(a)[0].lower() == os.path.splitdrive(b)[0].lower()
                and os.stat(a).st_dev == os.stat(b).st_dev)
    except OSError:
        return False


def vide(p):
    try:
        return not os.listdir(p)
    except OSError:
        return False


def destination(vers):
    """Dossier où ira l'installation, d'après le dossier choisi par le joueur."""
    vers = os.path.abspath(vers)
    if not os.path.exists(vers) or (os.path.isdir(vers) and vide(vers)):
        return vers
    if not os.path.isdir(vers):
        raise Echec(f"this is not a folder: {vers}")
    d = os.path.join(vers, NOM_DOSSIER)
    if os.path.exists(d) and not (os.path.isdir(d) and (vide(d) or os.path.exists(os.path.join(d, EN_COURS)))):
        raise Echec(f"this folder already exists and is not empty: {d}")
    return d


def controler(racine, dest):
    """Refus (Echec, en anglais) si l'installation ne peut pas aller dans dest. Rien n'est laissé sur le disque."""
    refus = desinstaller.installation(racine)
    if refus:   # dossier de sources : le message de desinstaller.py parle de suppression
        raise Echec(refus.split(": delete it yourself")[0] + (": the launcher does not move it" if "sources" in refus else ""))
    if norme(dest) == norme(racine):
        raise Echec("the game is already in this folder")
    if dedans(dest, racine):
        raise Echec("the new folder cannot be inside the current one")
    for var in ("ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "SystemRoot"):
        protege = os.environ.get(var)
        if protege and dedans(dest, protege):
            raise Echec(f"choose a folder that needs no administrator rights (not {protege})")
    try:   # même règle que preparer.py (verifier_pc) : le compilateur passe par les API ANSI de Windows
        dest.encode("mbcs", errors="strict")
    except UnicodeEncodeError:
        raise Echec("this folder's path contains characters the compiler cannot handle: choose another one "
                    "(for example C:\\Jeux\\StarFox)") from None
    if os.path.exists(dest) and not os.path.isdir(dest):
        raise Echec(f"this is not a folder: {dest}")
    if os.path.isdir(dest) and not vide(dest) and not os.path.exists(os.path.join(dest, EN_COURS)):
        raise Echec(f"this folder already exists and is not empty: {dest}")
    # essai d'écriture, sans rien laisser : les dossiers créés pour l'essai sont retirés
    base = existant(dest)
    try:
        os.makedirs(dest, exist_ok=True)
        essai = os.path.join(dest, ".essai_ecriture")
        with open(essai, "wb") as f:
            f.write(b"x")
        os.remove(essai)
    except OSError as e:
        raise Echec(f"cannot write in {dest} ({e.strerror or e})") from None
    finally:
        p = dest
        while norme(p) != norme(base) and len(p) > len(base):
            if os.path.isdir(p):
                if not vide(p):
                    break
                try:
                    os.rmdir(p)
                except OSError:
                    break
            p = os.path.dirname(p)


def fichiers(racine, exclus=()):
    """(chemin relatif, taille) de chaque fichier de l'installation, et la liste des dossiers (relatifs). Les fichiers
    de `exclus` (copies du jeu) et les marques de ce script sont sautés."""
    exclus = {norme(e) for e in exclus}
    base = long(racine)
    n = len(base) + 1
    liste, dossiers = [], []
    for d, sous, noms in os.walk(base):
        rel_d = d[n:]
        if rel_d:
            dossiers.append(rel_d)
        for f in noms:
            if not rel_d and f in (EN_COURS, PRET):
                continue
            rel = os.path.join(rel_d, f)
            if exclus and norme(os.path.join(racine, rel)) in exclus:
                continue
            try:
                liste.append((rel, os.path.getsize(os.path.join(d, f))))
            except OSError:
                liste.append((rel, 0))
    return liste, dossiers


def copier_fichier(src, dst, avance):
    """Copie avec les dates (shutil.copy2), par blocs pour suivre l'avancement. Quelques nouveaux essais : un
    antivirus peut tenir un fichier un instant."""
    for essai in range(6):
        try:
            with open(src, "rb") as a, open(dst, "wb") as b:
                while True:
                    bloc = a.read(BLOC)
                    if not bloc:
                        break
                    b.write(bloc)
                    avance(len(bloc))
            shutil.copystat(src, dst)
            return
        except OSError:
            if essai == 5:
                raise
            time.sleep(0.5)


def retirer(dossier):
    """Supprime un dossier de destination commencé. False s'il en reste."""
    for _ in range(10):
        restes = desinstaller.supprimer(dossier)
        if not os.path.exists(dossier):
            return True
        if not restes:
            break
        time.sleep(0.5)
    return not os.path.exists(dossier)


# ---------------------------------------------------------------------------------------------- commandes
def verifier(racine, vers):
    dest = destination(vers)
    controler(racine, dest)
    dire("@destination " + dest)
    dire("@volume " + ("meme" if meme_volume(racine, dest) else "autre"))


def copier(racine, dest):
    dire("@etapes dep_verifier,dep_copier")
    dire("@etape dep_verifier")
    controler(racine, dest)
    meme = meme_volume(racine, dest)
    copies = desinstaller.copies_du_jeu(racine)
    liste, dossiers = fichiers(racine, () if meme else copies)
    total = sum(t for _, t in liste)
    plus_long = max([len(rel) for rel, _ in liste] + [len(d) for d in dossiers] + [0])
    dire(f"@info {len(liste)} files, {go(total)}")
    # les outils (compilateur, glslang) ne lisent pas tous les chemins de plus de 259 caractères : pas plus longs qu'ici
    if len(dest) + 1 + plus_long > 259 and len(dest) > len(os.path.abspath(racine)):
        raise Echec(f"this folder's path is too long for the game's files ({len(dest) + 1 + plus_long} characters, "
                    "259 at most): choose a shorter one")
    if not meme:
        libre = shutil.disk_usage(existant(dest)).free
        besoin = total + max(1 << 30, total // 50)
        if libre < besoin:
            raise Echec(f"not enough free space on {os.path.splitdrive(dest)[0] or dest}: {go(besoin)} needed, "
                        f"{go(libre)} free")
    dire("@ok dep_verifier")
    dire("@etape dep_copier")
    if meme:
        dire("@info same drive: nothing to copy")
        dire("@progres 1.000")
        dire("@ok dep_copier")
        dire("@fini")
        return
    if os.path.exists(os.path.join(dest, EN_COURS)) and not retirer(dest):   # reste d'une copie interrompue
        raise Echec(f"cannot remove the unfinished copy in {dest}")
    os.makedirs(long(dest), exist_ok=True)
    with open(os.path.join(dest, EN_COURS), "w", encoding="utf-8") as f:
        f.write(os.path.abspath(racine) + "\n")
    for d in dossiers:
        os.makedirs(os.path.join(long(dest), d), exist_ok=True)
    fait, dernier = 0, 0.0

    def avance(n):
        nonlocal fait, dernier
        fait += n
        t = time.time()
        if t - dernier >= 0.2:
            dernier = t
            dire(f"@progres {min(fait / max(total, 1), 1.0) * 0.98:.4f}")
            dire(f"@info {go(fait)} / {go(total)}")

    for rel, _ in liste:
        src, dst = os.path.join(long(racine), rel), os.path.join(long(dest), rel)
        try:
            copier_fichier(src, dst, avance)
        except OSError as e:
            raise Echec(f"copy failed at {rel} ({e.strerror or e}); nothing was moved") from None
    for d in reversed(dossiers):   # dates des dossiers, une fois remplis
        try:
            shutil.copystat(os.path.join(long(racine), d), os.path.join(long(dest), d))
        except OSError:
            pass
    # contrôle : mêmes fichiers, mêmes tailles (un fichier que le launcher écrit encore peut avoir grandi : --finir
    # le recopie)
    manque = [rel for rel, _ in liste if not os.path.exists(os.path.join(long(dest), rel))]
    if manque:
        raise Echec(f"copy incomplete ({len(manque)} file(s) missing, for example {manque[0]}); nothing was moved")
    os.replace(os.path.join(dest, EN_COURS), os.path.join(dest, PRET))
    dire("@progres 1.000")
    dire(f"@info {go(total)} copied")
    dire("@ok dep_copier")
    dire("@fini")


def env_relance(statut):
    """Environnement du launcher relancé : le message à afficher (LANCEUR_STATUT : clé de textes.cpp), sans les
    variables de vérification qui referaient l'action. LANCEUR_DEPLACER_CAPTURE (vérification) : le launcher relancé
    écrit cette image puis se ferme."""
    env = dict(os.environ)
    for cle in ("LANCEUR_ACTION", "LANCEUR_DEPLACER_VERS", "LANCEUR_CAPTURE", "LANCEUR_CAPTURE_QUITTER"):
        env.pop(cle, None)
    env["LANCEUR_STATUT"] = statut
    if env.get("LANCEUR_DEPLACER_CAPTURE"):
        env["LANCEUR_CAPTURE"] = env["LANCEUR_DEPLACER_CAPTURE"]
        env["LANCEUR_CAPTURE_QUITTER"] = "1"
    return env


def relancer(dossier, statut):
    exe = os.path.join(dossier, "lanceur.exe")
    note(f"launcher restarted in {dossier} ({statut})")
    try:
        subprocess.Popen([exe], cwd=dossier, env=env_relance(statut), creationflags=0x00000008 | 0x00000200)
    except OSError as e:
        note(f"launcher not restarted: {e}")


def synchroniser(racine, dest, copies):
    """Recopie ce qui a changé dans l'origine depuis la copie (lanceur.ini, journaux écrits à la fermeture du
    launcher). Rend le nombre de fichiers recopiés."""
    liste, dossiers = fichiers(racine, copies)
    n = 0
    for d in dossiers:
        os.makedirs(os.path.join(long(dest), d), exist_ok=True)
    for rel, taille in liste:
        src, dst = os.path.join(long(racine), rel), os.path.join(long(dest), rel)
        try:
            s = os.stat(src)
            try:
                d = os.stat(dst)
                pareil = d.st_size == s.st_size and abs(d.st_mtime - s.st_mtime) < 2.0
            except OSError:
                pareil = False
            if not pareil:
                copier_fichier(src, dst, lambda _: None)
                n += 1
        except OSError as e:
            raise Echec(f"copy failed at {rel} ({e.strerror or e})") from None
    return n


def renommer_apres(racine, dest):
    """Même volume : PowerShell attend la fin de ce Python (il est dans le dossier), renomme le dossier, puis relance
    le launcher : au nouvel endroit, ou à l'ancien si le renommage échoue (fichier tenu par un autre programme)."""
    q = lambda s: "'" + s.replace("'", "''") + "'"   # noqa: E731
    lancer = ("function Lancer($d, $s) {{ $p = New-Object System.Diagnostics.ProcessStartInfo; "
              "$p.FileName = Join-Path $d 'lanceur.exe'; $p.WorkingDirectory = $d; $p.UseShellExecute = $false; "
              "$p.EnvironmentVariables['LANCEUR_STATUT'] = $s; [void][System.Diagnostics.Process]::Start($p) }}")
    cmd = (lancer.format()
           + f"; Wait-Process -Id {os.getpid()} -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 300"
           + "; $ok = $false; $err = ''"
           + "; for ($i = 0; $i -lt 40 -and -not $ok; $i++) { try { "
           + f"[System.IO.Directory]::Move({q(racine)}, {q(dest)}); $ok = $true"
           + " } catch { $err = $_.Exception.Message; Start-Sleep -Milliseconds 500 } }"
           + f"; $j = {q(JOURNAL)}; $t = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'"
           + "; if ($ok) { Add-Content -LiteralPath $j -Value ($t + '  folder renamed') -Encoding UTF8; "
           + f"Lancer {q(dest)} 'dep_fini' }}"
           + " else { Add-Content -LiteralPath $j -Value ($t + '  folder not renamed: ' + $err) -Encoding UTF8; "
           + f"Lancer {q(racine)} 'dep_echec' }}")
    subprocess.Popen(["powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", cmd],
                     cwd=tempfile.gettempdir(), env=env_relance("dep_fini"),
                     creationflags=0x00000200 | 0x08000000,   # pas DETACHED_PROCESS : PowerShell ne ferait rien
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def finir(racine, dest, pid):
    note(f"move: {racine} -> {dest}")
    desinstaller.attendre(pid)
    for _ in range(20):   # le launcher vient de se fermer : son exécutable peut rester tenu un instant
        qui = desinstaller.en_marche(racine)
        if not qui:
            break
        time.sleep(0.5)
    if qui:
        note(f"{qui} is running: nothing moved")
        return relancer(racine, "dep_echec") or 1
    refus = desinstaller.installation(racine)
    if refus:
        note("refused: " + refus)
        return 1
    if os.path.exists(os.path.join(dest, PRET)):   # autre volume : la copie est faite
        copies = desinstaller.copies_du_jeu(racine)
        try:
            n = synchroniser(racine, dest, copies)
        except Echec as e:
            note(f"last files not copied ({e}): the old folder is kept, nothing deleted")
            return relancer(racine, "dep_echec") or 1
        note(f"{n} file(s) copied again after the launcher closed")
        os.remove(os.path.join(dest, PRET))
        moi = norme(sys.executable)
        restes = [r for r in desinstaller.supprimer(racine, copies) if norme(r) != moi]
        if copies:
            note(f"old folder kept with your game copy: {os.path.dirname(copies[0])}")
        if restes:
            note(f"{len(restes)} file(s) in use left in the old folder, for example {restes[0]}")
        relancer(dest, "dep_fini_copie" if copies else "dep_fini")
        return 0
    if os.path.exists(os.path.join(dest, EN_COURS)):
        note("unfinished copy in the new folder: nothing moved")
        return relancer(racine, "dep_echec") or 1
    if not meme_volume(racine, dest):
        note("other drive and no finished copy: nothing moved")
        return relancer(racine, "dep_echec") or 1
    try:
        controler(racine, dest)
        if os.path.isdir(dest):
            os.rmdir(dest)   # vide (controler) : le renommage veut une destination absente
        os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
    except (Echec, OSError) as e:
        note(f"nothing moved: {e}")
        return relancer(racine, "dep_echec") or 1
    renommer_apres(racine, dest)
    return 0


def abandonner(dest):
    """Retire une destination commencée. Seulement avec la marque de ce script, et jamais le dossier d'où il tourne."""
    if dedans(LANCEUR, dest) or dedans(sys.executable, dest):
        note(f"abandon refused: this script runs from {dest}")
        return 1
    if not (os.path.exists(os.path.join(dest, EN_COURS)) or os.path.exists(os.path.join(dest, PRET))):
        return 0
    time.sleep(1.0)   # la copie vient d'être arrêtée : son dernier fichier peut être encore ouvert
    ok = retirer(dest)
    note(f"unfinished copy removed: {dest}" if ok else f"unfinished copy not fully removed: {dest}")
    return 0 if ok else 1


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--verifier", action="store_true")
    g.add_argument("--copier", action="store_true")
    g.add_argument("--finir", action="store_true")
    g.add_argument("--abandonner", action="store_true")
    ap.add_argument("--racine", default=os.path.dirname(os.path.dirname(LANCEUR)))
    ap.add_argument("--vers", required=True)
    ap.add_argument("--pid", type=int, default=0)
    a = ap.parse_args()
    racine, vers = os.path.abspath(a.racine), os.path.abspath(a.vers)
    try:
        if a.verifier:
            verifier(racine, vers)
        elif a.copier:
            copier(racine, vers)
        elif a.finir:
            return finir(racine, vers, a.pid)
        else:
            return abandonner(vers)
    except Echec as e:
        dire(f"@erreur {e}")
        return 1
    except Exception as e:  # noqa: BLE001  (disque, droits…)
        dire(f"@erreur {type(e).__name__}: {e}")
        note(f"error: {type(e).__name__}: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
