import re
import sys
import csv

import argparse

def setup_argparse():
  parser = argparse.ArgumentParser()
  parser.add_argument('--latencies', required=True, help='filename containing latency measurements')
  parser.add_argument('--runtime', required=True, help='runtime')
  parser.add_argument('--energies', required=False, help='File containing energy measurements')

  return parser

def parse_log(latencies, runtime, energies):
  start, end = -1.0, -1.0
  
  duration = float(runtime)
  warmup = duration/3.0
  
  tLatency = []
  sLatency = []
  fLatency = []
  
  tExtra = 0.0
  sExtra = 0.0
  fExtra = 0.0
  
  xLatency = []
  
  for line in open(latencies):
    if line.startswith('#') or line.strip() == "":
      continue
    
    line = line.strip().split()
    if not line[0].isdigit() or len(line) < 4 or re.search("[a-zA-Z]", "".join(line)):
      continue
    
    if start == -1:
      start = float(line[2]) + warmup
      end = start + warmup
  
    fts = float(line[2])
    
    if fts < start:
      continue
    
    if fts > end:
      break
    
    latency = int(line[3])
    status = int(line[4])
    ttype = -1
    try:
      ttype = int(line[5])
      extra = int(line[6])
    except:
      extra = 0
  
    if status == 1 and ttype == 2:
      xLatency.append(latency)
  
    tLatency.append(latency) 
    tExtra += extra
  
    if status == 1:
      sLatency.append(latency)
      sExtra += extra
    else:
      fLatency.append(latency)
      fExtra += extra
  
  if len(tLatency) == 0:
    print("Zero completed transactions..")
    sys.exit()
  
  tLatency.sort()
  sLatency.sort()
  fLatency.sort()

  energy_used = 0
  if energies is not None:
    with open(energies) as csvfile:
      reader = csv.DictReader(csvfile)
      start_entry = None
      end_entry = None

      for row in reader:
        if float(row["timestamp"]) < start:
          continue
        if float(row["timestamp"]) > end:
          break

        if start_entry is None:
          start_entry = row
        end_entry = row


      energy_used = float(end_entry["energy_joule"]) - float(start_entry["energy_joule"])
  
  print("Transactions(All/Success): ", len(tLatency), len(sLatency))
  print("Abort Rate: ", (float)(len(tLatency)-len(sLatency))/len(tLatency))
  print("Throughput (All/Success): ", len(tLatency)/(end-start), len(sLatency)/(end-start))
  print("Average Latency (all): ", sum(tLatency)/float(len(tLatency)))
  print("Median  Latency (all): ", tLatency[int(len(tLatency)/2)])
  print("99%tile Latency (all): ", tLatency[int((len(tLatency) * 99)/100)])
  print("Average Latency (success): ", sum(sLatency)/float(len(sLatency)))
  print("Median  Latency (success): ", sLatency[int(len(sLatency)/2)])
  print("99%tile Latency (success): ", sLatency[int((len(sLatency) * 99)/100)])
  print("Extra (all): ", tExtra)
  print("Extra (success): ", sExtra)
  if len(xLatency) > 0:
    print("X Transaction Latency: ", sum(xLatency)/float(len(xLatency)))
  if len(fLatency) > 0:
    print("Average Latency (failure): ", sum(fLatency)/float(len(tLatency)-len(sLatency)))
    print("Extra (failure): ", fExtra)
  if energies is not None:
    print("Energy used (J): ", energy_used)

def main():
  parser = setup_argparse()
  args = parser.parse_args()

  parse_log(args.latencies, args.runtime, args.energies)

if __name__ == "__main__":
  main()
