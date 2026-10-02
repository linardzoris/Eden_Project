#!/usr/bin/python
import sys, re, os, fnmatch, collections, string, chardet

REGEX_INCLUDE_FILE = re.compile(r"\s*(?:#include\s*\"([^\"]+)\")|\s*(?:#include\s*\<([^\"]+)\>)")

def fixcase(path):
  (dirname, filename) = os.path.split(path)
  if dirname and filename:
    try:
        fixed_filename = [f for f in os.listdir(dirname) if f.lower() == filename.lower()][0]
        return os.path.join(fixcase(dirname), fixed_filename)
    except:
        print("ERROR ", path, dirname, filename)
  else:
    return filename or dirname
  
def find_in_paths(include, include_paths):
    for include_path in include_paths:
        full_path = os.path.abspath(os.path.join(include_path, include))
        if os.path.exists(full_path):
            fixed_case = fixcase(full_path)
            return os.path.relpath(fixed_case, include_path).replace('\\','/')
    return None
    
def fix_include_case(source_file, include_paths):

  source_dir = os.path.dirname(source_file)
  include_paths_with_local = include_paths + [source_dir]
  e = chardet.detect(open(source_file, 'rb').read())['encoding']
  with open(source_file, 'r', encoding=e) as f:
    try:
        lines = f.readlines()
    except:
        print("ERROR reading file ", source_file, e)
        raise 
  changed = False
  for ln in range(len(lines)):
      line = lines[ln]
      match = REGEX_INCLUDE_FILE.match(line)
      if match:
        include = match.group(1) or match.group(2)
        (left_delim, right_delim) = ('"', '"') if match.group(1) else ('<', '>')
        found = find_in_paths(include, include_paths_with_local)
        if found and found != include:
            if not changed:
                print(source_file)
            changed = True
            newline = "#include " + left_delim + found + right_delim + "\n"
            print(line + " ==> " + newline)
            lines[ln] = newline
  if changed:
    with open(source_file, 'w', encoding=e) as f:
      for line in lines:
        f.write(line)
        

def main(argv):
  if len(argv) < 2:
      print("Usage: " + argv[0] + " <directory> <include_paths>")
      sys.exit(2)
  root_dir = argv[1]
  include_paths = argv[2:]
  for root, dirnames, filenames in os.walk(root_dir):
    for filename in filenames:
      if filename.endswith(('.h', '.cpp')):
        fix_include_case(os.path.join(root, filename), include_paths)
        
if __name__ == "__main__":
   main(sys.argv)