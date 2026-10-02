#!/usr/bin/python

import sys, os, chardet       

def main(argv):
  if len(argv) < 3:
      print("Usage: " + argv[0] + " <directory> encoding")
      sys.exit(2)
  root_dir = argv[1]
  target_encoding = argv[2]
  for root, dirnames, filenames in os.walk(root_dir):
    for filename in filenames:
      if filename.endswith(('.h', '.cpp', '.c', '.cc')):
        source_file = os.path.join(root, filename)
        source_encoding = chardet.detect(open(source_file, 'rb').read())['encoding']
        if source_encoding != target_encoding and source_encoding != 'ascii':
            print("Convert ", source_file, " from ", source_encoding)
            with open(source_file, 'r', encoding=source_encoding) as f:
                try:
                    lines = f.readlines()
                except:
                    print("ERROR reading file ", source_file, source_encoding)
                    raise
            with open(source_file, 'w', encoding=target_encoding) as f:
                try:
                    for line in lines:
                        f.write(line)
                except:
                    print("ERROR writing file ", source_file, source_encoding)
                    raise

if __name__ == "__main__":
   main(sys.argv)