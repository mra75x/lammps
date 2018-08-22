/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include <mpi.h>
#include <cstring>
#include "server_md.h"
#include "atom.h"
#include "atom_vec.h"
#include "update.h"
#include "integrate.h"
#include "kspace.h"
#include "force.h"
#include "pair.h"
#include "neighbor.h"
#include "comm.h"
#include "domain.h"
#include "error.h"

// CSlib interface

#include "cslib.h"

using namespace LAMMPS_NS;
using namespace CSLIB_NS;

enum{REAL,METAL}
enum{SETUP=1,STEP};
enum{DIM=1,PERIODICITY,ORIGIN,BOX,NATOMS,NTYPES,TYPES,COORDS,CHARGE};
enum{FORCES=1,ENERGY,VIRIAL};

// NOTE: features that could be added to this interface
// allow client to set periodicity vs shrink-wrap
//   currently just assume server is same as client
// test that triclinic boxes actually work
// send new box size/shape every step, for NPT client
// unit check between client/server with unit conversion if needed
// option for client to send other per-atom quantities, e.g. rmass
// more precise request of energy/virial (global or peratom) by client
//   maybe Verlet should have a single(eflag,vflag) method to more easily comply

/* ---------------------------------------------------------------------- */

ServerMD::ServerMD(LAMMPS *lmp) : Pointers(lmp)
{
  if (domain->box_exist == 0)
    error->all(FLERR,"Server command before simulation box is defined");

  if (!atom->map_style) error->all(FLERR,"Server md requires atom map");
  if (atom->tag_enable == 0) error->all(FLERR,"Server md requires atom IDs");
  if (sizeof(tagint) != 4) error->all(FLERR,"Server md requires 4-byte atom IDs");

  if (strcmp(update->unit_style,"real") == 0) units = REAL;
  else if (strcmp(update->unit_style,"metal") == 0) units = METAL;
  else error->all(FLERR,"Units must be real or metal for server md");
}

/* ---------------------------------------------------------------------- */

void ServerMD::loop()
{
  int i,j,m;

  // cs = instance of CSlib

  CSlib *cs = (CSlib *) lmp->cslib;

  // counters

  int forcecalls = 0;
  int neighcalls = 0;

  // loop on messages
  // receive a message, process it, send return message

  int msgID,nfield;
  int *fieldID,*fieldtype,*fieldlen;

  while (1) {
    msgID = cs->recv(nfield,fieldID,fieldtype,fieldlen);
    if (msgID < 0) break;

    // SETUP call at beginning of each run
    // required fields: NATOMS, NTYPES, BOXLO, BOXHI, TYPES, COORDS
    // optional fields: others in enum above

    if (msgID == SETUP) {

      int dim = 0;
      int *periodicity = NULL;
      int natoms = -1;
      int ntypes = -1;
      double *origin = NULL;
      double *box = NULL;
      int *types = NULL;
      double *coords = NULL;
      double *charge = NULL;

      for (int ifield = 0; ifield < nfield; ifield++) {
        if (fieldID[ifield] == DIM) {
          dim = cs->unpack_int(DIM);
          if (dim != domain->dimension)
            error->all(FLERR,"Server md dimension mis-match with client");
        } else if (fieldID[ifield] == PERIODICITY) {
          periodicity = (int *) cs->unpack(PERIODICITY);
          if (periodicity[0] != domain->periodicity[0] ||
              periodicity[1] != domain->periodicity[1]
              periodicity[2] != domain->periodicity[2])
            error->all(FLERR,"Server md periodicity mis-match with client");
        } else if (fieldID[ifield] == ORIGIN) {
          origin = (double *) cs->unpack(ORIGIN);
        } else if (fieldID[ifield] == BOX) {
          box = (double *) cs->unpack(BOX);
        } else if (fieldID[ifield] == NATOMS) {
          natoms = cs->unpack_int(NATOMS);
          if (3*natoms > INT_MAX)
            error->all(FLERR,"Server md max atoms is 1/3 of 2^31");
        } else if (fieldID[ifield] == NTYPES) {
          ntypes = cs->unpack_int(NTYPES);
          if (ntypes != atom->ntypes)
            error->all(FLERR,"Server md ntypes mis-match with client");
        } else if (fieldID[ifield] == TYPES) {
          types = (int *) cs->unpack(TYPES);
        } else if (fieldID[ifield] == COORDS) {
          coords = (double *) cs->unpack(COORDS);
        } else if (fieldID[ifield] == CHARGE) {
          charge = (double *) cs->unpack(CHARGE);
        } else error->all(FLERR,"Server md setup field unknown");
      }

      if (dim == 0 || !periodicity || !origin || !box || 
          natoms < 0 || ntypes < 0 || !types || !coords)
        error->all(FLERR,"Required server md setup field not received");

      if (charge && atom->q_flag == 0)
        error->all(FLERR,"Server md does not define atom attribute q");

      // reset box, global and local
      // reset proc decomposition
 
      if ((box[3] != 0.0 || box[6] != 0.0 || box[7] != 0.0) && 
          domain->triclinic == 0)
        error->all(FLERR,"Server md is not initialized for a triclinic box");

      box_change(origin,box);

      domain->set_initial_box();
      domain->set_global_box();
      comm->set_proc_grid();
      domain->set_local_box();

      // clear all atoms

      atom->nlocal = 0;
      atom->nghost = 0;

      // add atoms that are in my sub-box

      int nlocal = 0;
      for (int i = 0; i < natoms; i++) {
        if (!domain->ownatom(i+1,&coords[3*i],NULL,0)) continue;
        atom->avec->create_atom(types[i],&coords[3*i]);
        atom->tag[nlocal] = i+1;
        if (charge) atom->q[nlocal] = charge[i];
        nlocal++;
      }

      int ntotal;
      MPI_Allreduce(&atom->nlocal,&ntotal,1,MPI_INT,MPI_SUM,world);
      if (ntotal != natoms) 
        error->all(FLERR,"Server md atom count does not match client");

      atom->map_init();
      atom->map_set();
      atom->natoms = natoms;

      // perform system setup() for dynamics
      // also OK for minimization, since client runs minimizer
      // return forces, energy, virial to client

      update->whichflag = 1;
      lmp->init();
      update->integrate->setup_minimal(1);
      neighcalls++;
      forcecalls++;

      send_fev(msgID);

    // STEP call at each timestep of run or minimization
    // required fields: COORDS
    // optional fields: BOXLO, BOXHI, BOXTILT

    } else if (msgID == STEP) {

      double *coords = NULL;
      double *origin = NULL;
      double *box = NULL;

      for (int ifield = 0; ifield < nfield; ifield++) {
        if (fieldID[ifield] == COORDS) {
          coords = (double *) cs->unpack(COORDS);
        } else if (fieldID[ifield] == ORIGIN) {
          origin = (double *) cs->unpack(ORIGIN);
        } else if (fieldID[ifield] == BOX) {
          box = (double *) cs->unpack(BOX);
        } else error->all(FLERR,"Server md step field unknown");
      }

      if (!coords)
        error->all(FLERR,"Required server md step field not received");

      // change box size/shape, only if origin and box received
      // reset global/local box like FixDeform at end_of_step()

      if (origin && box) {
        if ((box[3] != 0.0 || box[6] != 0.0 || box[7] != 0.0) && 
            domain->triclinic == 0)
          error->all(FLERR,"Server md is not initialized for a triclinic box");
        box_change(origin,box);
        domain->set_global_box();
        domain->set_local_box();
      }

      // assign received coords to owned atoms
      // closest_image() insures xyz matches current server PBC

      double **x = atom->x;
      int nlocal = atom->nlocal;
      int nall = atom->natoms;

      j = 0;
      for (tagint id = 1; id <= nall; id++) {
        m = atom->map(id);
        if (m < 0 || m >= nlocal) j += 3;
        else {
          domain->closest_image(x[m],&coords[j],x[m]);
          j += 3;
        }
      }

      // if no need to reneighbor:
      //   ghost comm
      //   setup_minimal(0) which just computes forces
      // else:
      //   setup_minimal(1) for pbc, reset_box, reneigh, forces

      int nflag = neighbor->decide();
      if (nflag == 0) {
        comm->forward_comm();
        update->integrate->setup_minimal(0);
      } else {
        update->integrate->setup_minimal(1);
        neighcalls++;
      }

      forcecalls++;

      send_fev(msgID);

    } else error->all(FLERR,"Server MD received unrecognized message");
  }

  // final reply to client

  cs->send(0,0);

  // stats

  if (comm->me == 0) {
    if (screen) {
      fprintf(screen,"Server MD calls = %d\n",forcecalls);
      fprintf(screen,"Server MD reneighborings = %d\n",neighcalls);
    }
    if (logfile) {
      fprintf(logfile,"Server MD calls = %d\n",forcecalls);
      fprintf(logfile,"Server MD reneighborings %d\n",neighcalls);
    }
  }

  // clean up

  delete cs;
  lmp->cslib = NULL;
}

/* ----------------------------------------------------------------------
   box change due to received message
------------------------------------------------------------------------- */

void ServerMD::box_change(double *origin, double *box)
{
  domain->boxlo[0] = origin[0];
  domain->boxlo[1] = origin[1];
  domain->boxlo[2] = origin[2];

  domain->boxhi[0] = origin[0] + box[0];
  domain->boxhi[1] = origin[1] + box[4];
  domain->boxhi[2] = origin[2] + box[8];

  domain->xy = box[3];
  domain->xz = box[6];
  domain->yz = box[7];


}

/* ----------------------------------------------------------------------
   send return message with forces, energy, pressure tensor
   pressure tensor should be just pair style virial
------------------------------------------------------------------------- */

void ServerMD::send_fev(int msgID)
{
  CSlib *cs = (CSlib *) lmp->cslib;

  cs->send(msgID,3);
  
  double *forces = NULL;
  if (atom->nlocal) forces = &atom->f[0][0];
  cs->pack_parallel(FORCES,4,atom->nlocal,atom->tag,3,forces);
  
  double eng = force->pair->eng_vdwl + force->pair->eng_coul;
  double engall;
  MPI_Allreduce(&eng,&engall,1,MPI_DOUBLE,MPI_SUM,world);
  cs->pack_double(ENERGY,engall);
  
  double v[6],vall[6];
  for (int i = 0; i < 6; i++)
    v[i] = force->pair->virial[i];
  if (force->kspace)
    for (int i = 0; i < 6; i++)
      v[i] += force->kspace->virial[i];
  
  MPI_Allreduce(&v,&vall,6,MPI_DOUBLE,MPI_SUM,world);
  cs->pack(VIRIAL,4,6,vall);
}

