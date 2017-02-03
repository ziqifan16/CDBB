#!/bin/bash -l
#PBS -N cdbb-368
#PBS -e cdbb-368.e
#PBS -o cdbb-368.o
#PBS -l walltime=00:60:00,nodes=46:ppn=8
#PBS -M ziqifan16@gmail.com
#PBS -m abe
#PBS -q batch

module load intel impi
cd /home/dudh/fanxx234/CDBB
mpirun -np 368 ./cdbb
