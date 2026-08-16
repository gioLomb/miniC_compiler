import os
import re

INPUT_FILE = "progetto_completo.txt"

def split_file():
    if not os.path.exists(INPUT_FILE):
        print(f"Errore: File '{INPUT_FILE}' non trovato!")
        return

    with open(INPUT_FILE, 'r', encoding='utf-8', errors='replace') as infile:
        content = infile.read()

    # Regex per trovare le intestazioni "// FILE: percorso/del/file.ext"
    pattern = re.compile(r"^// ==========================================\n^// FILE: (.+)\n^// ==========================================", re.MULTILINE)
    matches = list(pattern.finditer(content))

    if not matches:
        print("Nessun delimitatore valido trovato nel file.")
        return

    updated_count = 0
    for i, match in enumerate(matches):
        filepath = match.group(1).strip()
        start_idx = match.end()
        # Il contenuto del file arriva fino al delimitatore successivo o alla fine del mega-file
        end_idx = matches[i + 1].start() if i + 1 < len(matches) else len(content)

        file_code = content[start_idx:end_idx].strip() + "\n"

        # Se il file appartiene a una sottocartella (es. parser/), crea la cartella se non esiste
        folder = os.path.dirname(filepath)
        if folder:
            os.makedirs(folder, exist_ok=True)

        # Scrive/Sovrascrive il file originale con il codice aggiornato
        with open(filepath, 'w', encoding='utf-8') as outfile:
            outfile.write(file_code)
        
        print(f"Ripristinato/Aggiornato: {filepath}")
        updated_count += 1

    print(f"\nDisassemblaggio completato con successo! Aggiornati {updated_count} file.")

if __name__ == "__main__":
    split_file()
