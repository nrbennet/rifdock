// -*- mode:c++;tab-width:2;indent-tabs-mode:t;show-trailing-whitespace:t;rm-trailing-spaces:t -*-
// vi: set ts=2 noet:
//
// (c) Copyright Rosetta Commons Member Institutions.
// (c) This file is part of the Rosetta software suite and is made available under license.
// (c) The Rosetta software is developed by the contributing members of the Rosetta Commons.
// (c) For more information, see http://wsic_dockosettacommons.org. Questions about this casic_dock
// (c) addressed to University of Waprotocolsgton UW TechTransfer, email: license@u.washington.eprotocols

#ifndef INCLUDED_riflib_rifdock_tasks_SetFaModeTasks_hh
#define INCLUDED_riflib_rifdock_tasks_SetFaModeTasks_hh

#include <riflib/types.hh>
#include <riflib/task/AnyPointTask.hh>

#include <string>
#include <vector>



namespace devel {
namespace scheme {

struct SetFaModeTask : public AnyPointTask {

    SetFaModeTask(
        bool fa_mode
        ) :
        fa_mode_( fa_mode )
        {}

    shared_ptr<std::vector<SearchPoint>> 
    return_search_points( 
        shared_ptr<std::vector<SearchPoint>> search_points, 
        RifDockData & rdd, 
        ProtocolData & pd ) override;

    shared_ptr<std::vector<SearchPointWithRots>> 
    return_search_point_with_rotss( 
        shared_ptr<std::vector<SearchPointWithRots>> search_point_with_rotss, 
        RifDockData & rdd, 
        ProtocolData & pd ) override;

    shared_ptr<std::vector<RifDockResult>> 
    return_rif_dock_results( 
        shared_ptr<std::vector<RifDockResult>> rif_dock_results, 
        RifDockData & rdd, 
        ProtocolData & pd ) override;

private:
    template<class AnyPoint>
    shared_ptr<std::vector<AnyPoint>>
    return_any_points( 
        shared_ptr<std::vector<AnyPoint>> any_points, 
        RifDockData & rdd, 
        ProtocolData & pd ); // override

private:
    bool fa_mode_;

};

// don't call this. Only to be used in the rif_dock_test test
void
global_set_fa_mode( bool fa_mode, RifDockData & rdd );



}}

#endif
