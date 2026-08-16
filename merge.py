import os

OUTPUT_FILE = "progetto_completo.txt"
# Estensioni incluse
EXTENSIONS = ('.c', '.h', '.re')

def merge_files():
    total_files = 0
    with open(OUTPUT_FILE, 'w', encoding='utf-8') as outfile:
        # os.walk naviga ricorsivamente in tutte le sottocartelle (parser, scanner, ecc.)
        for root, _, files in os.walk('.'):
            for file in files:
                if file.endswith(EXTENSIONS):
                    filepath = os.path.join(root, file)
                    
                    # Ignora il file di output se già presente per evitare loop
                    if os.path.abspath(filepath) == os.path.abspath(OUTPUT_FILE):
                        continue

                    # Normalizza il percorso relativo (es. parser/lexer.re o ./main.c -> main.c)
                    relpath = os.path.normpath(filepath)
                    
                    # Intestazione chiara per il parser di split.py
                    outfile.write(f"// ==========================================\n")
                    outfile.write(f"// FILE: {relpath}\n")
                    outfile.write(f"// ==========================================\n\n")
                    
                    try:
                        with open(filepath, 'r', encoding='utf-8', errors='replace') as infile:
                            outfile.write(infile.read())
                        total_files += 1
                        print(f"Aggiunto: {relpath}")
                    except Exception as e:
                        print(f"Errore nella lettura di {relpath}: {e}")
                    
                    outfile.write("\n\n")

    print(f"\nCompletato! Uniti {total_files} file in '{OUTPUT_FILE}'.")

if __name__ == "__main__":
    merge_files()
