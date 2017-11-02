// -*- mode:c++;tab-width:2;indent-tabs-mode:t;show-trailing-whitespace:t;rm-trailing-spaces:t -*-
// vi: set ts=2 noet:

// INC
#include <rif_dock_test.hh>
	#include <numeric/random/random.hh>

	#include <ObjexxFCL/format.hh>

	#include <boost/foreach.hpp>
	#include <boost/lexical_cast.hpp>
	// #include <boost/random/mersenne_twister.hpp>

	#include <core/id/AtomID.hh>
	#include <core/import_pose/import_pose.hh>
	#include <core/pose/Pose.hh>
	#include <core/pose/PDBInfo.hh>
	#include <core/pose/util.hh>
	#include <core/scoring/EnergyGraph.hh>
	#include <core/scoring/ScoreFunction.hh>
	#include <core/scoring/ScoreFunctionFactory.hh>
	#include <core/scoring/hbonds/HBondOptions.hh>
	#include <core/scoring/methods/EnergyMethodOptions.hh>
	#include <core/conformation/ResidueFactory.hh>
	#include <protocols/simple_moves/MinMover.hh>
	#include <core/kinematics/MoveMap.hh>
	#include <core/scoring/Energies.hh>

	#include <devel/init.hh>
	#include <riflib/RotamerGenerator.hh>
	#include <riflib/rosetta_field.hh>
	#include <riflib/util.hh>
	#include <riflib/rotamer_energy_tables.hh>

	// #include <numeric/alignment/QCP_Kernel.hh>
	#include <parallel/algorithm>
	#include <exception>
	#include <stdexcept>

	#include <scheme/actor/Atom.hh>
	#include <scheme/actor/BackboneActor.hh>
	#include <scheme/actor/VoxelActor.hh>
	#include <scheme/kinematics/Director.hh>
	#include <scheme/kinematics/SceneBase.hh>
	#include <scheme/nest/pmap/OriTransMap.hh>
	#include <scheme/numeric/rand_xform.hh>
	// #include <scheme/objective/ObjectiveFunction.hh>
	#include <scheme/objective/voxel/FieldCache.hh>
	// #include <scheme/objective/voxel/VoxelArray.hh>
	// #include <scheme/objective/hash/XformMap.hh>
	// #include <scheme/objective/storage/RotamerScores.hh>
	#include <scheme/util/StoragePolicy.hh>
	#include <scheme/search/HackPack.hh>
	#include <scheme/objective/integration/SceneObjective.hh>

	#include <riflib/RifFactory.hh>

	#include <utility/file/file_sys_util.hh>
	#include <utility/io/izstream.hh>
	#include <utility/io/ozstream.hh>

	#include <chrono>
	#include <random>


/// Brian
	#include <scheme/objective/hash/XformHash.hh>


// refactor
	#include <riflib/rifdock_subroutines/util.hh>
	#include <riflib/rifdock_subroutines/clustering.hh>
	#include <riflib/rifdock_subroutines/output_results.hh>



using ::scheme::make_shared;
using ::scheme::shared_ptr;

typedef int32_t intRot;



int main(int argc, char *argv[]) {

	#ifdef USE_OPENMP
		omp_lock_t cout_lock, dump_lock;
		omp_init_lock( &cout_lock );
		omp_init_lock( &dump_lock );
	#endif

	register_options();
	devel::init(argc,argv);

	using namespace core::scoring;
		using std::cout;
		using std::endl;
		using namespace devel::scheme;
		typedef numeric::xyzVector<core::Real> Vec;
		typedef numeric::xyzMatrix<core::Real> Mat;
		// typedef numeric::xyzTransform<core::Real> Xform;
		using ObjexxFCL::format::F;
		using ObjexxFCL::format::I;
		using devel::scheme::print_header;
		using ::devel::scheme::RotamerIndex;

	/////////////////////////////////////////////////////////////////////////////////
	/////////////////////// static shit
	////////////////////////////////////////////////////////////////////////////////
	typedef ::scheme::util::SimpleArray<3,float> F3;
	typedef ::scheme::util::SimpleArray<3,int> I3;


		typedef ::scheme::nest::NEST< 6,
									  EigenXform,
									  ::scheme::nest::pmap::OriTransMap,
									  ::scheme::util::StoreNothing, // do not store a transform in the Nest
									  uint64_t,
									  float,
									  false // do not inherit from NestBase
									 > NestOriTrans6D;

		typedef ::scheme::kinematics::NestDirector< NestOriTrans6D > DirectorOriTrans6D;
		typedef shared_ptr< ::scheme::kinematics::Director<EigenXform> > DirectorBase;


	print_header( "setup global options" );
		RifDockOpt opt;
		opt.init_from_cli();
		utility::file::create_directory_recursive( opt.outdir );

		::scheme::search::HackPackOpts packopts;
		packopts.pack_n_iters         = opt.pack_n_iters;
		packopts.pack_iter_mult       = opt.pack_iter_mult;
		packopts.hbond_weight         = opt.hbond_weight;
		packopts.upweight_iface       = opt.upweight_iface;
		packopts.upweight_multi_hbond = opt.upweight_multi_hbond;
		packopts.use_extra_rotamers   = opt.extra_rotamers;
		packopts.always_available_rotamers_level = opt.always_available_rotamers_level;
		packopts.packing_use_rif_rotamers = opt.packing_use_rif_rotamers;
		packopts.add_native_scaffold_rots_when_packing = opt.add_native_scaffold_rots_when_packing;
		packopts.rotamer_inclusion_threshold = -0.5;
		packopts.rotamer_onebody_inclusion_threshold = 5.0;
		packopts.init_with_best_1be_rots = true;
		packopts.user_rotamer_bonus_constant=opt.user_rotamer_bonus_constant;
		packopts.user_rotamer_bonus_per_chi=opt.user_rotamer_bonus_per_chi;

		std::string const rif_type = get_rif_type_from_file( opt.rif_files.back() );
		BOOST_FOREACH( std::string fn, opt.rif_files ){
			std::string rif_type2 = get_rif_type_from_file( fn );
			runtime_assert_msg( rif_type==rif_type2, "mismatched rif types, expect: " + rif_type + " got: " + rif_type2 + " for " + fn );
		}
		std::cout << "read RIF type: " << rif_type << std::endl;

		cout << "Search Resls: " << opt.resl0;
			std::vector<float> RESLS(1,opt.resl0);
			for( int i = 1; i < opt.rif_files.size(); ++i ){
				RESLS.push_back( RESLS.back()/2.0 );
				cout << " " << RESLS.back();
			}
			cout << endl;

		std::cout << "opt.rosetta_score_fraction: " << opt.rosetta_score_fraction << std::endl;
		std::cout << "opt.rosetta_score_then_min_below_thresh: " << opt.rosetta_score_then_min_below_thresh << std::endl;
		std::cout << "opt.rosetta_score_at_least: " << opt.rosetta_score_at_least << std::endl;
		std::cout << "opt.rosetta_score_at_most: " << opt.rosetta_score_at_most << std::endl;
		std::cout << "opt.rosetta_min_fraction: " << opt.rosetta_min_fraction << std::endl;
		std::cout << "opt.rosetta_min_targetbb: " << opt.rosetta_min_targetbb << std::endl;
		std::cout << "opt.rosetta_min_allbb: " << opt.rosetta_min_allbb << std::endl;
		std::cout << "opt.rosetta_score_cut: " << opt.rosetta_score_cut << std::endl;

		std::cout << "//////////////////////////// end options /////////////////////////////////" << std::endl;



		// for( int iscaff = 0; iscaff < opt.scaffold_fnames.size(); ++iscaff )
		// {
		// 	std::string scaff_fname = opt.scaffold_fnames.at(iscaff);
		// 	std::cout << scaff_fname << std::endl;
		// 	core::pose::Pose scaffold;
		// 	utility::vector1<core::Size> scaffold_res;
		// 	core::import_pose::pose_from_file(scaffold, scaff_fname);
		// 	scaff_fname = utility::file::file_basename(utility::file_basename(scaff_fname));
		// 	scaffold.dump_pdb(scaff_fname+"_0.pdb");
		// 	scaffold_res = devel::scheme::get_designable_positions_best_guess( scaffold, opt.dont_use_scaffold_loops );
		// 	::devel::scheme::pose_to_ala( scaffold, scaffold_res );
		// 	scaffold.dump_pdb(scaff_fname+"_1.pdb");
		// }
		// utility_exit_with_message("test_scaff sel");


		////////////////////////////// should be no more use of options at this point! ///////////////////////////


		double time_rif=0, time_pck=0, time_ros=0;

		std::mt19937 rng( std::random_device{}() );


		{
			std::string dokfile_fname_orig = opt.dokfile_fname;
			int i = 2;
			while( utility::file::file_exists(opt.dokfile_fname) ){
				opt.dokfile_fname = dokfile_fname_orig + "." + str(i);
				++i;
			}
			if( i != 2)
				std::cout << "WARNING!" << dokfile_fname_orig << " already exists, using "
			              << opt.dokfile_fname << " instead!" << std::endl;
             else
             	std::cout << "output scores to " << opt.dokfile_fname << std::endl;
		}
		utility::io::ozstream dokout( opt.dokfile_fname );


		devel::scheme::RifFactoryConfig rif_factory_config;
		rif_factory_config.rif_type = rif_type;
		shared_ptr<RifFactory> rif_factory = ::devel::scheme::create_rif_factory( rif_factory_config );


		// shared_ptr<RifFactory> rif_factory;
		// if ( opt.scaffold_provider_type == "SingleFile" ) {
		// 	rif_factory = ::devel::scheme::create_rif_factory<SingleFileScaffoldProvider>(rif_factory_config);
		// } else if ( opt.scaffold_provider_type == "Alex" ) {
		// 	rif_factory = ::devel::scheme::create_rif_factory<MorphingScaffoldProvider>(rif_factory_config);
		// } else {
		// 	utility_exit_with_message( "rif_dock_test: unknown scaffold provider type "+opt.scaffold_provider_type );
		// }




	print_header( "create rotamer index" );
		shared_ptr< RotamerIndex > rot_index_p = make_shared< RotamerIndex >();
		RotamerIndex & rot_index( *rot_index_p );
		::devel::scheme::get_rotamer_index( rot_index, opt.extra_rotamers, opt.extra_rif_rotamers );

		// {
		// 	utility::io::ozstream out("test.rotidx.gz",std::ios_base::binary);
		// 	rot_index.save(out);
		// 	out.close();
		// }
		// {
		// 	RotamerIndex ri2;
		// 	utility::io::izstream in("test.rotidx.gz",std::ios_base::binary);
		// 	ri2.load(in);
		// 	in.close();

		// 	std::cout << rot_index << std::endl;
		// 	std::cout << std::endl;
		// 	std::cout << ri2 << std::endl;

		// 	runtime_assert( ri2 == rot_index );
		// 	utility_exit_with_message("test rot index load/save");
		// }

		// std::cout << "================ RotamerIndex ===================" << std::endl;
		// std::cout << rot_index.size() << " " << rot_index.n_primary_rotamers() << std::endl;
		// std::cout << rot_index << std::endl;
		// {
		// 	utility::io::ozstream out("rot_index.pdb");
		// 	rot_index.dump_pdb( out );
		// 	utility_exit_with_message("ortsdn");
		// }
		// std::cout << "=================================================" << std::endl;

		RotamerRFOpts rotrfopts;
		rotrfopts.oversample     = opt.rotrf_oversample;
		rotrfopts.field_resl     = opt.rotrf_resl;
		rotrfopts.field_spread   = opt.rotrf_spread;
		rotrfopts.data_dir       = opt.rotrf_cache_dir;
		rotrfopts.scale_atr      = opt.rotrf_scale_atr;
		::devel::scheme::RotamerRFTablesManager rotrf_table_manager( rot_index_p, rotrfopts );
		// rotrf_table_manager.preinit_all();




	///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	print_header( "read and prepare target structure" ); //////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	core::pose::Pose target;
	std::vector<SimpleAtom> target_simple_atoms;
	utility::vector1<core::Size> target_res;
	std::vector<HBondRay> target_donors, target_acceptors;
	float rif_radius=0.0, target_redundancy_filter_rg=0.0;
	{
		core::import_pose::pose_from_file( target, opt.target_pdb );

		if( opt.use_scaffold_bounding_grids ){
			for( int ir = 1; ir <= target.size(); ++ir ){
				utility::vector1<core::Size> resids(1,ir); // 1-index numbering
				std::vector<SchemeAtom> atoms;
				devel::scheme::get_scheme_atoms( target, resids, atoms );
				int restype = rot_index.chem_index_.resname2num( target.residue(ir).name3() ); // for UNK will be -1
				for( int ia = 0; ia < atoms.size(); ++ia){
					SchemeAtom const & a( atoms[ia] );
					runtime_assert( a.type() > 0 );
					if( a.type() >= 21 ) continue;
					SimpleAtom sa( a.position(), a.type(), restype, ia );
					target_simple_atoms.push_back(sa);
				}
			}
			std::cout << "target_simple_atoms.size() " << target_simple_atoms.size() << std::endl;
		}
		target_res = devel::scheme::get_res( opt.target_res_fname , target, /*nocgp*/false );
		get_rg_radius( target, target_redundancy_filter_rg, rif_radius, target_res, true ); // allatom for target
		rif_radius += 7.0; // hacky guess
		::devel::scheme::HBRayOpts hbopt;
		// hbopt.withbb = true;
		// hbopt.lkball = true;
		// hbopt.add_acceptor_mid = true;
		// hbopt.satisfied_atoms = ::devel::scheme::get_satisfied_atoms(target);
// rif score:    0 rank         0 dist0:      20.44 packscore: -30.391 steric:  -1.096 cluster:       0 rifrank:  268480 0.04808 test_ful
// rif score:    1 rank         1 dist0:      20.30 packscore: -29.679 steric:   1.352 cluster:       0 rifrank:  535822 0.09596 test_ful
// rif score:    2 rank         2 dist0:      20.30 packscore: -29.463 steric:  -0.926 cluster:       0 rifrank:  295509 0.05292 test_ful
// rif score:    3 rank         3 dist0:      19.78 packscore: -29.328 steric:   1.009 cluster:       0 rifrank:  163867 0.02935 test_ful
// rif score:    4 rank         4 dist0:      19.74 packscore: -28.592 steric:  -1.015 cluster:       0 rifrank:  151703 0.02717 test_ful
// rif score:    5 rank         5 dist0:      19.78 packscore: -28.512 steric:  -0.840 cluster:       0 rifrank:   63255 0.01133 test_ful
// rif score:    6 rank         6 dist0:      19.52 packscore: -28.471 steric:   3.097 cluster:       0 rifrank:   83780 0.01500 test_ful
// rif score:    7 rank         7 dist0:      20.40 packscore: -28.236 steric:  -0.913 cluster:       0 rifrank:  409585 0.07336 test_ful
// rif score:    8 rank         8 dist0:      20.30 packscore: -27.996 steric:  -1.081 cluster:       0 rifrank:  172518 0.03090 test_ful
// rif score:    9 rank         9 dist0:      19.88 packscore: -27.802 steric:   1.592 cluster:       0 rifrank:  435730 0.07804 test_ful
// rif score:   10 rank        10 dist0:      20.05 packscore: -27.500 steric:   0.539 cluster:       0 rifrank:  426173 0.07633 test_ful
// rif score:   11 rank        11 dist0:      20.30 packscore: -27.491 steric:  -0.683 cluster:       0 rifrank:   52799 0.00946 test_ful
// rif score:   12 rank        12 dist0:      19.88 packscore: -27.474 steric:   0.449 cluster:       0 rifrank:  309114 0.05536 test_ful
// rif score:   13 rank        13 dist0:      19.88 packscore: -27.209 steric:  -0.946 cluster:       0 rifrank:  152655 0.02734 test_ful
// rif score:   14 rank        14 dist0:      20.17 packscore: -27.184 steric:  -0.464 cluster:       0 rifrank:  116844 0.02093 test_ful
// rif score:   15 rank        15 dist0:      20.70 packscore: -27.138 steric:  -1.345 cluster:       0 rifrank:  398589 0.07139 test_ful
// rif score:   16 rank        16 dist0:      20.17 packscore: -26.992 steric:   2.271 cluster:       0 rifrank:  353244 0.06326 test_ful
// rif score:   17 rank        17 dist0:      20.00 packscore: -26.914 steric:   1.221 cluster:       0 rifrank:  787232 0.14099 test_ful
// rif score:   18 rank        18 dist0:      19.69 packscore: -26.807 steric:   1.547 cluster:       0 rifrank:  336496 0.06027 test_ful
// rif score:   19 rank        19 dist0:      20.14 packscore: -26.777 steric:  -1.035 cluster:       0 rifrank:  295196 0.05287 test_ful
		// hbopt.withbb = true;
		// hbopt.lkball = true;
		// hbopt.add_acceptor_mid = false;
		hbopt.satisfied_atoms = ::devel::scheme::get_satisfied_atoms(target);

		// utility_exit_with_message("MAKE SURE LKBALL STUFF ISN'T FUCKING UP!!!");


		BOOST_FOREACH( core::Size ir, target_res ){
			::devel::scheme::get_donor_rays   ( target, ir, hbopt, target_donors );
			::devel::scheme::get_acceptor_rays( target, ir, hbopt, target_acceptors );
		}
		std::cout << "target_donors.size() " << target_donors.size() << " target_acceptors.size() " << target_acceptors.size() << std::endl;
		// {
		// 	{
		// 		std::vector<HBondRay> tmpdon, tmpacc, tmpacclk;
		// 		::devel::scheme::HBRayOpts hbopt;
		// 		hbopt.lkball = false;
		// 		hbopt.withbb = true;
		// 		hbopt.add_acceptor_mid = true;
		// 		hbopt.satisfied_atoms = ::devel::scheme::get_satisfied_atoms(target);
		// 		BOOST_FOREACH( core::Size ir, target_res ){
		// 			::devel::scheme::get_donor_rays   ( target, ir, hbopt, tmpdon );
		// 			::devel::scheme::get_acceptor_rays( target, ir, hbopt, tmpacc );
		// 		}
		// 		hbopt.lkball = true;
		// 		BOOST_FOREACH( core::Size ir, target_res ){
		// 			::devel::scheme::get_acceptor_rays( target, ir, hbopt, tmpacclk );
		// 		}
		// 		target.dump_pdb("target.pdb");
		// 		utility::io::ozstream donout(utility::file_basename(opt.target_pdb)+"_donors.pdb");
		// 		::devel::scheme::dump_hbond_rays( donout, tmpdon, true );
		// 		donout.close();
		// 		utility::io::ozstream accout(utility::file_basename(opt.target_pdb)+"_orb_acceptors.pdb");
		// 		::devel::scheme::dump_hbond_rays( accout, tmpacc, false );
		// 		accout.close();
		// 		utility::io::ozstream accoutlk(utility::file_basename(opt.target_pdb)+"_lkb_acceptors.pdb");
		// 		::devel::scheme::dump_hbond_rays( accoutlk, tmpacclk, false );
		// 		accoutlk.close();
		// 		utility_exit_with_message("testing lkball replace orbs");
		// 	}
		// }
	}
	std::vector< VoxelArrayPtr > target_field_by_atype;
	std::vector< std::vector< VoxelArrayPtr > > target_bounding_by_atype;
	{
		target_bounding_by_atype.resize( RESLS.size() );
		devel::scheme::RosettaFieldOptions rfopts;
		rfopts.field_resl = opt.target_rf_resl;
		rfopts.data_dir = "DUMMY_DATA_DIR_FIXME";
		rfopts.oversample = opt.target_rf_oversample;
		rfopts.block_hbond_sites = false;
		rfopts.max_bounding_ratio = opt.max_rf_bounding_ratio;
		rfopts.fail_if_no_cached_data = true;
		rfopts.repulsive_only_boundary = true;
		rfopts.cache_mismatch_tolerance = 0.01; // this is kinda loose...
		std::string cache_prefix = opt.target_rf_cache;
		devel::scheme::get_rosetta_fields_specified_cache_prefix(
			cache_prefix,
			opt.target_pdb,
			target,
			target_res,
			rfopts,
			target_field_by_atype,
			false
		);


		if( true ){
			// std::cout << "using target bounding grids, generating (or loading) them" << std::endl;
			devel::scheme::RosettaFieldOptions rfopts;
			rfopts.field_resl = opt.target_rf_resl;
			rfopts.data_dir = "DUMMY_DATA_DIR_FIXME";
			rfopts.oversample = opt.target_rf_oversample;
			rfopts.block_hbond_sites = false;
			rfopts.max_bounding_ratio = opt.max_rf_bounding_ratio;
			rfopts.fail_if_no_cached_data = true;
			rfopts.repulsive_only_boundary = true; // default
			devel::scheme::get_rosetta_bounding_fields_from_fba(
				RESLS,
				opt.target_pdb,
				target,
				target_res,
				rfopts,
				target_field_by_atype,
				target_bounding_by_atype,
				false,
				cache_prefix
			);
			runtime_assert( target_bounding_by_atype.size() == RESLS.size() );
			// now scale down the any positive component by 1/RESL if RESL > 1
			if( opt.downscale_atr_by_hierarchy ){
				std::cout << "downscale_atr_by_hierarchy on target bounding steric grids" << std::endl;
				// std::cout << "  zeroing atr component of target bounding steric grids" << std::endl;
				for( int iresl = 0; iresl < RESLS.size(); ++iresl ){
					float correction = 1.0/RESLS[iresl];
					if( correction >= 1.0 ) break;
					BOOST_FOREACH( VoxelArrayPtr vap, target_bounding_by_atype[iresl] ){
						if( vap != nullptr ){
							std::exception_ptr exception = nullptr;
							#ifdef USE_OPENMP
							#pragma omp parallel for schedule(dynamic,64)
							#endif
							for( int k = 0; k < vap->num_elements(); ++k ){
								if( exception ) continue;
								try {
									float & dat = vap->data()[k];
									if( dat < 0 ){
										dat = dat * correction;
										// dat = 0; // for testing w/o attractive sterics
									}
								} catch( std::exception const & ex ) {
									#pragma omp critical
									exception = std::current_exception();
								}
							}
							if( exception ) std::rethrow_exception(exception);
						}
					}
				}
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	print_header( "read in RIFs" ); /////////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	std::vector<shared_ptr<RifBase> > rif_ptrs;
	std::vector<bool> rif_using_rot;
	{
		std::vector<std::string> rif_descriptions( opt.rif_files.size() );
		rif_ptrs.resize( opt.rif_files.size() );
		std::exception_ptr exception = nullptr;
		#ifdef USE_OPENMP
		#pragma omp parallel for schedule(dynamic,1)
		#endif
		for( int i_readmap = 0; i_readmap < opt.rif_files.size(); ++i_readmap ){
			if( exception ) continue;
			try {
				std::string const & rif_file = opt.rif_files[i_readmap];
				std::string & rif_dscr = rif_descriptions[i_readmap];
				shared_ptr<RifBase> & rif_ptr = rif_ptrs[i_readmap];
				rif_ptr = rif_factory->create_rif_from_file( rif_file, rif_dscr );
				runtime_assert_msg( rif_ptrs[i_readmap] , "rif creation from file failed! " + rif_file );
				if( opt.VERBOSE ){
					#ifdef USE_OPENMP
					#pragma omp critical
					#endif
					std::cout << "================= read " << rif_file << "=================" << std::endl
					          << "description:" << std::endl << rif_dscr << std::endl
					          << "load factor: " << rif_ptr->load_factor() << std::endl;
				}
				#ifdef USE_OPENMP
				#pragma omp critical
				#endif
				std::cout << "loaded RIF score for resl " << F(7,3,RESLS[i_readmap])
				          << " raw cart_resl: " << F(7,3,rif_ptr->cart_resl() )
				          << " raw ang_resl: " << F(7,3,rif_ptr->ang_resl() ) << std::endl;

				if (i_readmap == opt.rif_files.size() -1 ) {
					// #pragma omp criticial
					// rif_ptr->super_print( std::cout, rot_index_p );
					// std::ofstream out_file;
					// out_file.open("rif.txt");
					// rif_ptr->super_print( out_file, rot_index_p );
					// out_file.close();

				}
			} catch( std::exception const & ex ) {
				#ifdef USE_OPENMP
				#pragma omp critical
				#endif
				exception = std::current_exception();
			}
		}
		if( exception ) std::rethrow_exception(exception);

		std::cout << "RIF description:" << std::endl << rif_descriptions.back() << std::endl;
		std::cout << "load factor: " << rif_ptrs.back()->load_factor() << std::endl;
		std::cout << "size of value-type: " << rif_ptrs.back()->sizeof_value_type() << std::endl;
		std::cout << "mem_use: " << ::devel::scheme::KMGT( rif_ptrs.back()->mem_use() ) << std::endl;
		std::cout << "===================================================================================" << std::endl;

		rif_using_rot.resize( rot_index_p->size(), false );
		rif_using_rot[ rot_index.ala_rot() ] = true; // always include ala
		rif_ptrs.back()->get_rotamer_ids_in_use( rif_using_rot );
		int Nusingrot = 0;
		for( int i = 0; i < rif_using_rot.size(); ++i ){
			Nusingrot += rif_using_rot[i] ? 1 : 0;
		}
		std::cout << "rif uses: " << Nusingrot << " rotamers " << std::endl;
	}


    if( 0 == opt.scaffold_fnames.size() ){
        std::cout << "WARNING: NO SCAFFOLDS!!!!!!" << std::endl;
    }

	for( int iscaff = 0; iscaff < opt.scaffold_fnames.size(); ++iscaff )
	{
		std::string scaff_fname = opt.scaffold_fnames.at(iscaff);
		std::vector<std::string> scaffold_sequence_glob0;				// Scaffold sequence in name3 space
		utility::vector1<core::Size> scaffold_res;//, scaffold_res_all; // Seqposs of residues to design, default whole scaffold
		try {

			runtime_assert( rot_index_p );
			std::string scafftag = utility::file_basename( utility::file::file_basename( scaff_fname ) );

			std::cout << "/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////" << std::endl;
			std::cout << "/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////" << std::endl;
			std::cout << "//////   begin scaffold " << scafftag << " " << iscaff << " of " << opt.scaffold_fnames.size() << std::endl;
			std::cout << "/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////" << std::endl;
			std::cout << "/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////" << std::endl;

			core::pose::Pose scaffold;								// the input scaffold, gets converted to alanine with flags
			core::pose::Pose scaffold_centered;                     // input (maybe alanine) scaffold centered using scaffold_center
			core::pose::Pose scaffold_full_centered;				// input full aa scaffold centered using scaffold_center
			core::pose::Pose both_pose;								// scaffold (maybe alanine) centered + target (from rifgen)
			core::pose::Pose both_full_pose; 						// scaffold centered + target (from rifgen)
			core::pose::Pose scaffold_only_pose;				
			core::pose::Pose scaffold_only_full_pose;
			core::pose::Pose scaffold_unmodified_from_file;

			float scaff_radius = 0.0;
			float redundancy_filter_rg = 0.0;						// rg of scaffold to decide minimum angular resolution?

			std::vector<int> scaffres_g2l;							// maps global_seqpos -> local_seqpos  (local_seqpos.size() == scaffold_res.size())
			std::vector<int> scaffres_l2g;							// maps local_seqpos  -> global_seqpos
			std::vector<bool> scaffuseres;							// maps global_seqpos -> being_used
			Eigen::Vector3f scaffold_center;						// center of scaffold heavy atoms after conversion to alanine
			std::vector<Vec> scaffca;								// xyz coordinates of scaffold CA
			std::vector<std::vector<float> > scaffold_onebody_glob0;// onebody_rotamer_energies using global_seqpos
			std::vector<std::vector<float> > local_onebody;			// onebody_rotamer_energies using local_seqpos
			std::vector< std::pair<int,int> > local_rotamers;		// lower and upper bounds into rotamer_index for each local_seqpos
			typedef ::scheme::objective::storage::TwoBodyTable<float> TBT;

			shared_ptr<TBT> scaffold_twobody = make_shared<TBT>( scaffold.size(), rot_index.size()  );  // twobody_rotamer_energies using global_seqpos
			shared_ptr<TBT> local_twobody;							// twobody_rotamer_energies using local_seqpos

			EigenXform scaffold_perturb = EigenXform::Identity();
			{
				core::import_pose::pose_from_file( scaffold, scaff_fname );
				scaffold_unmodified_from_file = scaffold;
				if( opt.random_perturb_scaffold ){
					runtime_assert_msg( !opt.use_scaffold_bounding_grids,
						"opt.use_scaffold_bounding_grids incompatible with random_perturb_scaffold" );
					::scheme::numeric::rand_xform(rng,scaffold_perturb);
					xform_pose( scaffold, eigen2xyz(scaffold_perturb) );
				}

				scaffold_full_centered = scaffold;

				for( int ir = 1; ir <= scaffold.size(); ++ir ){
					scaffold_sequence_glob0.push_back( scaffold.residue(ir).name3() );
				}

				std::string scaff_res_fname = "";
				if( opt.scaffold_res_fnames.size() ){
					if( opt.scaffold_res_fnames.size() == opt.scaffold_fnames.size() ){
						scaff_res_fname = opt.scaffold_res_fnames.at(iscaff);
					} else if( opt.scaffold_res_fnames.size() == 1 ){
						scaff_res_fname = opt.scaffold_res_fnames.front();
					} else {
						utility_exit_with_message( "-scaffold_res list not same length as -scaffolds list" );
					}
					if( opt.scaffold_res_use_best_guess ){
						utility_exit_with_message("should only use -scaffold_res_use_best_guess true iff not specifying scaffold_res");
					}
					scaffold_res = devel::scheme::get_res( scaff_res_fname , scaffold );
				} else if (opt.scaffold_res_use_best_guess ){
					scaffold_res = devel::scheme::get_designable_positions_best_guess( scaffold, opt.dont_use_scaffold_loops );
					std::cout << "using scaffold residues: ";
					for(auto ir:scaffold_res) std::cout << " " << ir << scaffold.residue(ir).name3();
					std::cout << std::endl;
				} else {
					for( int ir = 1; ir <= scaffold.size(); ++ir){
						if( !scaffold.residue(ir).is_protein() ) continue;
						//if( scaffold.residue(ir).name3() == "PRO" ) continue;
						//if( scaffold.residue(ir).name3() == "GLY" ) continue;
						//if( scaffold.residue(ir).name3() == "CYS" ) continue;
						scaffold_res.push_back(ir);
					}
				}
				if     ( opt.scaff2ala )        ::devel::scheme::pose_to_ala( scaffold );
				else if( opt.scaff2alaselonly ) ::devel::scheme::pose_to_ala( scaffold, scaffold_res );
				std::cout << "rifdock scaffold_res: " << scaffold_res << std::endl;

				// scaffold.dump_pdb( utility::file_basename(scaff_fname)+"_pruned.pdb");

				float scaff_redundancy_filter_rg=0;
				get_rg_radius( scaffold, scaff_redundancy_filter_rg, scaff_radius, scaffold_res, false ); // not allatom for scaff
				redundancy_filter_rg = std::min( scaff_redundancy_filter_rg, target_redundancy_filter_rg );
				std::cout << "scaffold selected region rg: " << scaff_redundancy_filter_rg << ", radius: " << scaff_radius << std::endl;
				std::cout << "using redundancy_filter_rg: " << redundancy_filter_rg << std::endl;

				int count = 0;
				scaffres_g2l.resize(scaffold.size(),-1);
				scaffuseres .resize(scaffold.size(),false);
				for( auto ir : scaffold_res ){
					scaffres_g2l[ir-1] = count++;
					scaffres_l2g.push_back(ir-1);
					scaffuseres[ir-1] = true;
				}

				std::cout << "scaffold: " << scaff_fname << " nres: " << scaffold.size() << " using_res: " << scaffold_res.size() << std::endl;
				scaffold_center = pose_center(scaffold,scaffold_res);
				scaffold_centered = scaffold;
				for( int ir = 1; ir <= scaffold.size(); ++ir ){
					Vec tmp( scaffold_center[0], scaffold_center[1], scaffold_center[2] );
					for( int ia = 1; ia <= scaffold.residue_type(ir).natoms(); ++ia ){
						core::id::AtomID aid(ia,ir);
						scaffold_centered.set_xyz( aid, scaffold.xyz(aid) - tmp );
					}
					for( int ia = 1; ia <= scaffold_full_centered.residue_type(ir).natoms(); ++ia ){
						core::id::AtomID aid(ia,ir);
						scaffold_full_centered.set_xyz( aid, scaffold_full_centered.xyz(aid) - tmp );
					}
				}

				both_pose      = scaffold_centered;
				both_full_pose = scaffold_full_centered;
				scaffold_only_pose = scaffold_centered;
				scaffold_only_full_pose = scaffold_full_centered;
				::devel::scheme::append_pose_to_pose( both_pose, target );
				::devel::scheme::append_pose_to_pose( both_full_pose, target );
				runtime_assert( both_pose.size() == scaffold.size() + target.size() );
				runtime_assert( both_pose.size() == both_full_pose.size() );



				for( int ir = 1; ir <= scaffold.size(); ++ir ){
					scaffca.push_back( scaffold.residue(ir).xyz("CA") );
					// scaffold_res_all.push_back(ir);
				}
				std::string scaff_tag = utility::file_basename( scaff_fname );
				std::string scaff_res_hashstr = ::devel::scheme::get_res_list_hash( scaffold_res );
				std::string cachefile_1be = "__1BE_"+scaff_tag+(opt.replace_all_with_ala_1bre?"_ALLALA":"")+"_reshash"+scaff_res_hashstr+".bin.gz";
				if( ! opt.cache_scaffold_data ) cachefile_1be = "";
				std::cout << "rifdock: get_onebody_rotamer_energies" << std::endl;
				get_onebody_rotamer_energies(
						scaffold,
						scaffold_res,			// uses 12345 as score for anything missing here
						rot_index,
						scaffold_onebody_glob0,
						opt.data_cache_path,
						cachefile_1be,
						opt.replace_all_with_ala_1bre
					);

				if( opt.restrict_to_native_scaffold_res ){
					std::cout << "KILLING NON-NATIVE ROTAMERS ON SCAFFOLD!!!" << std::endl;
					for( int ir = 0; ir < scaffold_onebody_glob0.size(); ++ir ){
						for( int irot = 0; irot < rot_index.size(); ++irot ){
							if( rot_index.resname(irot) != scaffold_sequence_glob0.at(ir) && rot_index.resname(irot) != "ALA" ){
								scaffold_onebody_glob0[ir][irot] = 9e9;
							}
						}
					}
				}
				if( opt.bonus_to_native_scaffold_res != 0 ){
					std::cout << "adding to native scaffold res 1BE " << opt.bonus_to_native_scaffold_res << std::endl;
					for( int ir = 0; ir < scaffold_onebody_glob0.size(); ++ir ){
						for( int irot = 0; irot < rot_index.size(); ++irot ){
							if( rot_index.resname(irot) == scaffold_sequence_glob0.at(ir) ){
								scaffold_onebody_glob0[ir][irot] += opt.bonus_to_native_scaffold_res;
							}
						}
					}
				}

				for( int i = 0; i < scaffres_l2g.size(); ++i ){
					local_onebody.push_back( scaffold_onebody_glob0.at( scaffres_l2g.at(i) ) );
				}
				for( int i = 0; i < scaffres_g2l.size(); ++i ){
					if( scaffres_g2l[i] < 0 ){
						BOOST_FOREACH( float & f, scaffold_onebody_glob0[i] ) f = 9e9;
					}
				}


				std::cout << "rifdock: get_twobody_tables" << std::endl;
				std::string cachefile2b = "__2BE_" + scaff_tag + "_reshash" + scaff_res_hashstr + ".bin.gz";
				if( ! opt.cache_scaffold_data || opt.extra_rotamers ) cachefile2b = "";
				MakeTwobodyOpts make2bopts;
				// hacked by brian             VVVV
				make2bopts.onebody_threshold = 30.0;
				make2bopts.distance_cut = 15.0;
				make2bopts.hbond_weight = packopts.hbond_weight;
				std::string dscrtmp;
				get_twobody_tables(
						opt.data_cache_path,
						cachefile2b,
						dscrtmp,
						scaffold,
						rot_index,
						scaffold_onebody_glob0,
						rotrf_table_manager,
						make2bopts,
						*scaffold_twobody
					);
				std::cout << "rifdock: twobody memuse: " << (float)scaffold_twobody->twobody_mem_use()/1000.0/1000.0 << "M" << std::endl;

				{
					std::cout << "rifdock: onebody dimension: " << scaffold_onebody_glob0.size() << " " << scaffold_onebody_glob0.front().size() << std::endl;
					int onebody_n_allowed = 0;
					for( auto const & t : scaffold_onebody_glob0 ){
						for( auto const & v : t ){
							if( v < make2bopts.onebody_threshold ) onebody_n_allowed++;
						}
					}
					std::cout << "rifdock: onebody Nallowed: " << onebody_n_allowed << std::endl;
				}

				// // remove rotamers not seen in the rif... removed to test out extra-rotamers
				// for( int i = 0; i < scaffold_onebody_glob0.size(); ++i ){
				// 	runtime_assert( scaffold_onebody_glob0[i].size() == rot_index.size() );
				// 	for( int j = 0; j < scaffold_onebody_glob0[i].size(); ++j ){
				// 		scaffold_onebody_glob0[i][j] = rif_using_rot[j] ? scaffold_onebody_glob0[i][j] : 9e9;
				// 	}
				// }
				// for( int i = 0; i < local_onebody.size(); ++i ){
				// 	runtime_assert( local_onebody[i].size() == rot_index.size() );
				// 	for( int j = 0; j < local_onebody[i].size(); ++j ){
				// 		local_onebody[i][j] = rif_using_rot[j] ? local_onebody[i][j] : 9e9;
				// 	}
				// }

				local_rotamers.clear();
				for( int i = 0; i < local_onebody.size(); ++i ){
					int iresglobal = scaffres_l2g.at(i);
					std::string name3 = scaffold_sequence_glob0.at(iresglobal);
					std::pair<int,int> ib = rot_index.index_bounds( name3 );
					// std::cout << "local_rotamers " << i << " " << iresglobal << " " << name3 << " " << ib.first << " " << ib.second << std::endl;
					local_rotamers.push_back( ib );
				}
																					  //this was hacked  VV  by brian
				local_twobody = scaffold_twobody->create_subtable( scaffuseres, scaffold_onebody_glob0, 30 );
				std::cout << "filt_2b memuse: " << (float)local_twobody->twobody_mem_use()/1000.0/1000.0 << "M" << std::endl;
				std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << endl;
				std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! must fix issue with non-global 2B table calculation, seems to use scaffold_res when it shouldn't" << endl;
				std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << endl;

				// todo: prune twobody table???
			}

			// SOMETHING WRONG, SCORES OFF BY A LITTLE
			// setup objectives, moved into scaffold loop to guarantee clean slate for each scaff...
			RifSceneObjectiveConfig rso_config;
				rso_config.packopts = &packopts;
				rso_config.rif_ptrs = rif_ptrs;
				rso_config.target_bounding_by_atype = &target_bounding_by_atype;
				rso_config.target_field_by_atype = &target_field_by_atype;
				rso_config.local_onebody = &local_onebody;
				rso_config.local_rotamers = &local_rotamers;
				rso_config.local_twobody = local_twobody;
				rso_config.rot_index_p = rot_index_p;
				rso_config.target_donors = &target_donors;
				rso_config.target_acceptors = &target_acceptors;
				rso_config.n_sat_groups = 1000;//target_donors.size() + target_acceptors.size();
				rso_config.require_satisfaction = opt.require_satisfaction;
				rso_config.require_n_rifres = opt.require_n_rifres;

			ScenePtr scene_prototype;
			std::vector< ObjectivePtr > objectives;
			ObjectivePtr packing_objective;
			runtime_assert( rif_factory->create_objectives( rso_config, objectives, packing_objective ) );
			scene_prototype = rif_factory->create_scene();
			runtime_assert_msg( objectives.front()->is_compatible( *scene_prototype ), "objective and scene types not compatible!" );





			print_header( "setup 3D rosetta_field grids for scaffold" );
			std::vector< VoxelArrayPtr > scaffold_field_by_atype;
			std::vector< std::vector< VoxelArrayPtr > > scaffold_bounding_by_atype;
			std::vector< SimpleAtom > scaffold_simple_atoms, scaffold_simple_atoms_all;  // the CB atom of each scaffold residue
			if( opt.use_scaffold_bounding_grids ){
				scaffold_bounding_by_atype.resize( RESLS.size() );
				float const rf_resl = opt.rf_resl==0.0 ? RESLS.back()/2.0 : opt.rf_resl;
				devel::scheme::RosettaFieldOptions rfopts;
				rfopts.field_resl = rf_resl;
				rfopts.data_dir = "DUMMY_DATA_DIR_FIXME";
				rfopts.oversample = opt.rf_oversample;
				rfopts.block_hbond_sites = false;
				rfopts.max_bounding_ratio = opt.max_rf_bounding_ratio;
				rfopts.repulsive_only_boundary = true; // default
				devel::scheme::get_rosetta_bounding_fields(
					RESLS,
					scaff_fname+"_CEN"+(opt.scaff2ala?"_ALLALA":""),
					scaffold_centered,
					scaffold_res,
					rfopts,
					scaffold_field_by_atype,
					scaffold_bounding_by_atype,
					false
				);
				runtime_assert( scaffold_bounding_by_atype.size() == RESLS.size() );
				// now scale down the any positive component by 1/RESL if RESL > 1
				if( opt.downscale_atr_by_hierarchy ){
					std::cout << "downscale_atr_by_hierarchy on scaffold bounding steric grids" << std::endl;
					// std::cout << "zeroing atr component of scaffold steric grids" << std::endl;
					for( int iresl = 0; iresl < RESLS.size(); ++iresl ){
						float correction = 1.0/RESLS[iresl];
						if( correction >= 1.0 ) break;
						BOOST_FOREACH( VoxelArrayPtr vap, scaffold_bounding_by_atype[iresl] ){
							if( vap != nullptr ){
								#ifdef USE_OPENMP
								#pragma omp parallel for schedule(dynamic,64)
								#endif
								for( int k = 0; k < vap->num_elements(); ++k ){
									float & dat = vap->data()[k];
									if( dat < 0 ){
										dat = dat * correction;
									}
								}
							}
						}
					}
				}
			} else {
				std::cout << "not using scaffold bounding grids" << std::endl;
				for( int ir = 1; ir <= scaffold_centered.size(); ++ir ){
					utility::vector1<core::Size> resids(1,ir); // 1-index numbering
					{
						std::vector<SchemeAtom> scaff_res_atoms;
						if( !opt.lowres_sterics_cbonly && std::find( scaffold_res.begin(), scaffold_res.end(), ir ) != scaffold_res.end() ){
							devel::scheme::get_scheme_atoms( scaffold_centered, resids, scaff_res_atoms, true );
						} else { // is not selected residue
							devel::scheme::get_scheme_atoms_cbonly( scaffold_centered, resids, scaff_res_atoms );
						}
						int restype = rot_index.chem_index_.resname2num( scaffold_centered.residue(ir).name3() ); // for UNK will be -1
						for( int ia = 0; ia < scaff_res_atoms.size(); ++ia){
							SchemeAtom const & a( scaff_res_atoms[ia] );
							runtime_assert( a.type() > 0 );
							if( a.type() >= 21 ) continue;
							SimpleAtom sa( a.position(), a.type(), restype, ia );
							scaffold_simple_atoms.push_back(sa);
						}
					}
					{
						std::vector<SchemeAtom> all_scaff_res_atoms;
						devel::scheme::get_scheme_atoms( scaffold_centered, resids, all_scaff_res_atoms, false );
						int restype = rot_index.chem_index_.resname2num( scaffold_centered.residue(ir).name3() ); // for UNK will be -1
						for( int ia = 0; ia < all_scaff_res_atoms.size(); ++ia){
							SchemeAtom const & a( all_scaff_res_atoms[ia] );
							runtime_assert( a.type() > 0 );
							if( a.type() >= 21 ) continue;
							SimpleAtom sa( a.position(), a.type(), restype, ia );
							scaffold_simple_atoms_all.push_back(sa);
						}
					}
				}
				std::cout << "scaffold_simple_atoms " << scaffold_simple_atoms.size() << std::endl;

			}

			std::vector<EigenXform> symmetries_clash_check;
			if( opt.nfold_symmetry > 1 ){
				// utility_exit_with_message("NOT IMPLEMENTED!");
				// check only 1 and Nfold - 1, hense the strange += value
				// symmetries_clash_check.push_back( EigenXform::Identity() );
				for(int isym = 1; isym < opt.nfold_symmetry; isym += opt.nfold_symmetry-2){
					float angle_rads = isym * 2.0 * M_PI / opt.nfold_symmetry;
					Eigen::AngleAxis<typename EigenXform::Scalar> aa( angle_rads, Eigen::Vector3f(0,0,1) );
					EigenXform x(aa.toRotationMatrix());
					runtime_assert( x.translation().norm() < 0.0001 );
					symmetries_clash_check.push_back( x );
					std::cout << "USING SYMMETRY CLASH CHECK HACK!!!! " << isym << " " << angle_rads << std::endl;
				}
				if( !opt.use_scaffold_bounding_grids ){
					std::cout << "making atype 5-only scaffold bounding grids" << std::endl;
					// need to init scaffold*_by_atype for, say, atype 5, then use this for symmetrical clash checking
					scaffold_bounding_by_atype.resize( RESLS.size() );
					devel::scheme::RosettaFieldOptions rfopts;
					rfopts.field_resl = 1.0;
					rfopts.data_dir = "DUMMY_DATA_DIR_FIXME";
					rfopts.oversample = 1;
					rfopts.block_hbond_sites = false;
					rfopts.max_bounding_ratio = opt.max_rf_bounding_ratio;
					rfopts.repulsive_only_boundary = true; // default
					rfopts.one_atype_only = 5; // atype 5 only good enough for the sym clash check
					devel::scheme::get_rosetta_bounding_fields(
						RESLS,
						scaff_fname+"_CEN"+(opt.scaff2ala?"_ALLALA":""),
						scaffold_centered,
						scaffold_res,
						rfopts,
						scaffold_field_by_atype,
						scaffold_bounding_by_atype,
						false
					);
					std::cout << "done making atype 5-only scaffold bounding grids" << std::endl;
				}
			}


			///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			print_header( "setup scene from scaffold and target" );
			///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			ScenePtr scene_minimal( scene_prototype->clone_deep() );
			ScenePtr scene_full( scene_prototype->clone_deep() );
			{
				for( int ir = 1; ir <= scaffold.size(); ++ir ){
					Vec N  = scaffold_centered.residue(ir).xyz("N" );
					Vec CA = scaffold_centered.residue(ir).xyz("CA");
					Vec C  = scaffold_centered.residue(ir).xyz("C" );

					// todo map res indices, must also edit onebody_energies
					BBActor bbactor( N, CA, C, '-', '-', scaffres_g2l[ir-1] );
					runtime_assert( bbactor.index_ == scaffres_g2l[ir-1] );


					scene_full->add_actor(1,bbactor);
					if( std::find(scaffold_res.begin(),scaffold_res.end(),ir)!=scaffold_res.end() ){
						scene_minimal->add_actor(1,bbactor);
					}
				}

				if( opt.use_scaffold_bounding_grids ){
					BOOST_FOREACH( SimpleAtom const & sa, target_simple_atoms )	scene_minimal->add_actor( 0, sa );
					runtime_assert( scene_minimal->template num_actors<SimpleAtom>(0) == target_simple_atoms.size() );
					scene_minimal->add_actor( 1, VoxelActor(scaffold_bounding_by_atype) );
				} else {
					BOOST_FOREACH( SimpleAtom const & sa, scaffold_simple_atoms ) scene_minimal->add_actor( 1, sa );
					runtime_assert( scene_minimal->template num_actors<SimpleAtom>(1) == scaffold_simple_atoms.size() );
					scene_minimal->add_actor( 0, VoxelActor(target_bounding_by_atype) );
				}


			}
			cout << "scores for scaffold in original position: " << std::endl;
			// {
			// 	EigenXform x(EigenXform::Identity());
			// 	x.translation() = scaffold_center;
			// 	scene_minimal->set_position(1,x);
			// 	for(int i = 0; i < RESLS.size(); ++i){
			// 		std::vector<float> sc = objectives[i]->scores(*scene_minimal);
			// 		cout << "input bounding score " << i << " " << F(7,3,RESLS[i]) << " "
			// 		     << F( 7, 3, sc[0]+sc[1] ) << " "
			// 		     << F( 7, 3, sc[0]       ) << " "
			// 		     << F( 7, 3, sc[1]       ) << endl;
			// 	}

			// }

			// utility_exit_with_message("FOO");

			///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			print_header( "setup director based on scaffold and target sizes" ); //////////////////////////////////////////////////////////////////////////////////////////////
			///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			DirectorBase director; {
				F3 target_center = pose_center(target);
				float const body_radius = std::min( scaff_radius, rif_radius );
				double const cart_grid = opt.resl0*opt.hsearch_scale_factor/sqrt(3); // 1.5 is a big hack here.... 2 would be more "correct"
				double const hackysin = std::min( 1.0, opt.resl0*opt.hsearch_scale_factor/2.0/ body_radius );
				runtime_assert( hackysin > 0.0 );
				double const rot_resl_deg0 = asin( hackysin ) * 180.0 / M_PI;
				int nside = std::ceil( opt.search_diameter / cart_grid );
				std::cout << "search dia.    : " <<  opt.search_diameter << std::endl;
				std::cout << "nside          : " << nside        << std::endl;
				std::cout << "resl0:           " << opt.resl0 << std::endl;
				std::cout << "body_radius:     " << body_radius << std::endl;
				std::cout << "rif_radius:      " << rif_radius << std::endl;
				std::cout << "scaffold_radius: " << scaff_radius << std::endl;
				std::cout << "cart_grid:       " << cart_grid  << std::endl;
				std::cout << "rot_resl_deg0:   " << rot_resl_deg0 << std::endl;
				I3 nc( nside, nside, nside );
				F3 lb = target_center + F3( -cart_grid*nside/2.0, -cart_grid*nside/2.0, -cart_grid*nside/2.0 );
				F3 ub = target_center + F3(  cart_grid*nside/2.0,  cart_grid*nside/2.0,  cart_grid*nside/2.0 );
				std::cout << "cart grid ub " << ub << std::endl;
				std::cout << "cart grid lb " << lb << std::endl;
				std::cout << "(ub-lb/nc) = " << ((ub-lb)/nc.template cast<float>()) << std::endl;
				std::cout << "cartcen to corner (cart. covering radius): " << sqrt(3.0)*cart_grid/2.0 << std::endl;
				shared_ptr<DirectorOriTrans6D> director_concrete = make_shared<DirectorOriTrans6D>( rot_resl_deg0, lb, ub, nc, 1 );
				std::cout << "Director:" << endl << *director_concrete << endl;
				director = director_concrete;
				std::cout << "nest size0:    " << director->size(0) << std::endl;
				std::cout << "size of search space: ~" << float(director->size(0))*1024.0*1024.0*1024.0 << " grid points" << std::endl;
			}


//////////////////////////////////////////////////////////////////////////////////////


			// these are in rosetta numbers
			// int rmsd_resid1 = 107;
			// int rmsd_resid2 = 54;
			// int rmsd_resid3 = 13;


			// utility::vector1<core::Size> rmsd_resids { rmsd_resid1, rmsd_resid2, rmsd_resid3 };
			// std::vector<SchemeAtom> rmsd_atoms;
			// devel::scheme::get_scheme_atoms_cbonly( scaffold_unmodified_from_file, rmsd_resids, rmsd_atoms);
			
			// SchemeAtom rmsd_cb1 = rmsd_atoms[0];
			// SchemeAtom rmsd_cb2 = rmsd_atoms[1];
			// SchemeAtom rmsd_cb3 = rmsd_atoms[2];

			// std::cout << "Checking that we have the right CBetas for rmsd" << std::endl;
			// std::cout << I(5, rmsd_resid1) << " " << rmsd_cb1.position().transpose() << std::endl;
			// std::cout << I(5, rmsd_resid2) << " " << rmsd_cb2.position().transpose() << std::endl;
			// std::cout << I(5, rmsd_resid3) << " " << rmsd_cb3.position().transpose() << std::endl;


//////////////////////////////////////////////////////////////////////////////////////



			std::vector< SearchPointWithRots > packed_results;
			std::vector< ScenePtr > scene_pt( omp_max_threads_1() );
			int64_t non0_space_size = 0;
			int64_t npack = 0;
			int64_t total_search_effort = 0;
			bool search_failed = false;
			{
		        std::chrono::time_point<std::chrono::high_resolution_clock> start_rif = std::chrono::high_resolution_clock::now();

				///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
				print_header( "perform hierarchical search" ); ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
				///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
				std::vector< std::vector< SearchPoint > > samples( RESLS.size() );
				{
					samples[0].resize( director->size(0) );
					for( uint64_t i = 0; i < director->size(0); ++i ) samples[0][i] = SearchPoint( i );
					BOOST_FOREACH( ScenePtr & s, scene_pt ) s = scene_minimal->clone_shallow();
					for( int iresl = 0; iresl < RESLS.size(); ++iresl )
					{
						cout << "HSearsh stage " << iresl+1 << " resl " << F(5,2,RESLS[iresl]) << " begin threaded sampling, " << KMGT(samples[iresl].size()) << " samples: ";
						int64_t const out_interval = samples[iresl].size()/50;
						std::exception_ptr exception = nullptr;
					    std::chrono::time_point<std::chrono::high_resolution_clock> start, end;
				        start = std::chrono::high_resolution_clock::now();
				        total_search_effort += samples[iresl].size();

				        std::vector< double > rmsds( samples[iresl].size(), 1000 );

				        bool answer_exists = false;

						#ifdef USE_OPENMP
						#pragma omp parallel for schedule(dynamic,64)
						#endif
						for( int64_t i = 0; i < samples[iresl].size(); ++i ){
							if( exception ) continue;
							try {
								if( i%out_interval==0 ){ cout << '*'; cout.flush();	}
								uint64_t const isamp = samples[iresl][i].index;

///////////////////////////////////////////////////////////////////////////////////////////////

								// if ( isamp < 3000000000000000 && isamp != 47729600827993 && 
								// 	 isamp != 745775012937 && isamp != 11652734577 &&
								// 	 isamp != 182073977 && isamp != 2844905 ) {
								// 	continue;
								// }
								// if ( isamp > 3000000000000000 && isamp != 3054694452991568 ) {
								// 	continue;
								// }


								// money 3054694452991568
///////////////////////////////////////////////////////////////////////////////////////////////


								ScenePtr tscene( scene_pt[omp_get_thread_num()] );
								director->set_scene( isamp, iresl, *tscene );

								if( opt.tether_to_input_position ){
									EigenXform x = tscene->position(1);
									x.translation() -= scaffold_center;
									float xmag =  xform_magnitude( x, redundancy_filter_rg );
									if( xmag > opt.tether_to_input_position_cut + RESLS[iresl] ){
										samples[iresl][i].score = 9e9;
										continue;
									} else {
										// std::cout << "inbounds " << iresl << " " << xform_magnitude( tscene->position(1), redundancy_filter_rg ) << std::endl;
									}
								}

   								float tot_sym_score = 0;
                                if( opt.nfold_symmetry > 1 ){
									bool dump = false;
									if( iresl == 2 ) dump = true;
									if(dump){
									    scaffold_centered.dump_pdb("test_scaff.pdb");
									    target.dump_pdb("test_target.pdb");
									}
									EigenXform x = tscene->position(1);
									EigenXform xinv = x.inverse();
                                    for(int isym = 0; isym < symmetries_clash_check.size(); ++isym){
                                        EigenXform const & xsym = symmetries_clash_check[isym];
									// for( EigenXform const & xsym : symmetries_clash_check ){
                                        utility::io::ozstream * outp = nullptr;
                                        if(dump){
                                            outp = new utility::io::ozstream("test_sym_"+str(isym)+".pdb");
                                        }
										EigenXform x_to_internal = xinv * xsym * x;
										for( SimpleAtom a : scaffold_simple_atoms ){
											a.set_position( x_to_internal * a.position() );
		                            		if(dump) ::scheme::actor::write_pdb( *outp, a, rot_index_p->chem_index_ );
		                                    float atom_score = scaffold_bounding_by_atype.at(iresl).at(5)->at(a.position());
		                                 	if( atom_score > 0.0 ){ // clash only
			                                    tot_sym_score += atom_score;
		                                 	}
										}
                                        if(dump){
                                            outp->close();
                                            delete outp;
    									}
                                    }
									if(dump){
										std::cout << "tot_sym_score " << tot_sym_score << std::endl;
										utility_exit_with_message("testing symmetric clash check");
									}
                                }


                                // the real rif score!!!!!!
                                samples[iresl][i].score = objectives[iresl]->score( *tscene ) + tot_sym_score;




///////////////////////////////////////////////////////////////////////////////////////////

								// if ( isamp == 3054694452991568 || isamp == 47729600827993 ||
								// 	 isamp == 745775012937 || isamp == 11652734577 ||
								// 	 isamp == 182073977 || isamp == 2844905 ) {


        //                         	// samples[iresl][i].score = objectives[iresl]->score( *tscene ) + tot_sym_score;
                                
        //                         	// float score = objectives[iresl]->score( *tscene ) + tot_sym_score;
								// 	// answer_exists = true;

								// 	#pragma omp critical
								// 	std::cout << "Score for the one: " << F(6, 2, samples[iresl][i].score) << std::endl;
								// }



        //                         const SimpleAtom scene_cb1 = tscene->template get_actor<SimpleAtom>(1, rmsd_resid1-1);
        //                         const SimpleAtom scene_cb2 = tscene->template get_actor<SimpleAtom>(1, rmsd_resid2-1);
        //                         const SimpleAtom scene_cb3 = tscene->template get_actor<SimpleAtom>(1, rmsd_resid3-1);

        //                         const float cb1_dist2 = ( scene_cb1.position() - rmsd_cb1.position() ).squaredNorm();
        //                         const float cb2_dist2 = ( scene_cb2.position() - rmsd_cb2.position() ).squaredNorm();
        //                         const float cb3_dist2 = ( scene_cb3.position() - rmsd_cb3.position() ).squaredNorm();

        //                         const float rmsd_squared = cb1_dist2 + cb2_dist2 + cb3_dist2;
        //                         // const float rmsd = std::sqrt( (cb1_dist * cb1_dist + cb2_dist * cb2_dist + cb3_dist * cb3_dist) / 3.00 );

        //                         rmsds[i] = rmsd_squared;

        //                         samples[iresl][i].score = rmsd_squared - 200.0;

        //                         if (isamp > 3000000000000000) {
        //                         	// samples[iresl][i].score = objectives[iresl]->score( *tscene ) + tot_sym_score;
        //                         }


                                // if ( isamp == 47780569615988) {
                                // 	double score = objectives[iresl]->score( *tscene ) + tot_sym_score;
                                // 	std::cout << "Score for the one: " << score << std::endl;
                                // }

                               	// #pragma omp critical
                                // std::cout << I(20, isamp) << F(7, 1, cb1_dist) << F(7, 1, cb2_dist) << F(7, 1, cb3_dist) << std::endl;

                               	// SimpleAtom sa = tscene->template get_actor<SimpleAtom>(1,0);
                               	// SimpleAtom sa2 = tscene->template get_actor<SimpleAtom>(1,13);
                               	// bool success = true;
                               	// // // bool success = tscene->get_actor( 1, 0, sa ); 
                               	// if ( success ) {
                               	// 	#pragma omp critical
                               	// 	std::cout << "Succss: " << I(8, isamp) << " SA: " << sa << sa2 << std::endl;
                               	// } else {
                               	// 	#pragma omp critical
                               	// 	std::cout << "Failure" << std::endl;
                               	// }


////////////////////////////////////////////////////////////////////////////////////////////



							} catch( std::exception const & ex ) {
								#ifdef USE_OPENMP
								#pragma omp critical
								#endif
								exception = std::current_exception();
							}
						}
						if( exception ) std::rethrow_exception(exception);
				      	end = std::chrono::high_resolution_clock::now();
						std::chrono::duration<double> elapsed_seconds_rif = end-start;
						float rate = (double)samples[iresl].size()/ elapsed_seconds_rif.count()/omp_max_threads();
						cout << endl;// << "done threaded sampling, partitioning data..." << endl;

//////////////////////////////////////////////////////////////////////////////////////

						// shared_ptr<DirectorOriTrans6D> nest_director = std::dynamic_pointer_cast<DirectorOriTrans6D>(director);
						// NestOriTrans6D const & nest( nest_director->nest() );

						// std::cout << "Sorting rmsds... " << std::endl;
						// std::vector< uint64_t > sorted( rmsds.size() );
						// uint64_t n = 0;
						// std::generate( std::begin(sorted), std::end(sorted), [&]{ return n++; });
						// std::sort( std::begin( sorted), std::end(sorted),
						// 	[&](int i1, int i2) { return rmsds[i1] < rmsds[i2]; } );

						// for ( uint64_t i = 0; i < 64; i++ ) {
						// 	uint64_t index = sorted[i];
						// 	SearchPoint sp = samples[iresl][index];
						// 	uint64_t isamp = sp.index;


						// 	EigenXform trans;
						// 	bool success = nest.get_state( isamp, iresl, trans );

						// 	Eigen::Matrix<float,3,3> rot;
						// 	scheme::objective::hash::get_transform_rotation<float>( trans, rot );

						// 	// Eigen::Matrix<double,3,3> rot2 = rot.cast<double>();

						// 	Eigen::Vector3f axis;
						// 	axis.setRandom();
						// 	axis.normalize();
						// 	Eigen::AngleAxisf angle_axis( rot );


						// 	std::cout << I(20, isamp) << F(7, 1, rmsds[index]) << "  Trans: " 
						// 	<< F(7, 1, trans.translation()[0]) 
						// 	<< F(7, 1, trans.translation()[1]) 
						// 	<< F(7, 1, trans.translation()[2]) 
						// 	<< "  Angle: " << F(7, 1, angle_axis.angle()*180.0/M_PI) 
						// 	<< "  Score: " << F(7, 2, sp.score)

						// 	<< std::endl;
						// }
////////////////////////////////////////////////////////////////////////////////////////


						SearchPoint max_pt, min_pt;
						int64_t len = samples[iresl].size();
						if( samples[iresl].size() > opt.beam_size/opt.DIMPOW2 ){
							__gnu_parallel::nth_element( samples[iresl].begin(), samples[iresl].begin()+opt.beam_size/opt.DIMPOW2, samples[iresl].end() );
							len = opt.beam_size/opt.DIMPOW2;
							min_pt = *__gnu_parallel::min_element( samples[iresl].begin(), samples[iresl].begin()+len );
							max_pt = *(samples[iresl].begin()+opt.beam_size/opt.DIMPOW2);
						} else {
							min_pt = *__gnu_parallel::min_element( samples[iresl].begin(), samples[iresl].end() );
							max_pt = *__gnu_parallel::max_element( samples[iresl].begin(), samples[iresl].end() );
						}

						cout << "HSearsh stage " << iresl+1 << " complete, resl. " << F(7,3,RESLS[iresl]) << ", "
							  << " " << KMGT(samples[iresl].size()) << ", promote: " << F(9,6,min_pt.score) << " to "
							  << F(9,6, std::min(opt.global_score_cut,max_pt.score)) << " rate " << KMGT(rate) << "/s/t " << std::endl;

						// cout << "Answer: " << ( answer_exists ? "exists" : "doesn't exist" ) << std::endl;

						if( iresl+1 == samples.size() ) break;

						for( int64_t i = 0; i < len; ++i ){
							uint64_t isamp0 = samples[iresl][i].index;
							if( samples[iresl][i].score >= opt.global_score_cut ) continue;
							if( iresl == 0 ) ++non0_space_size;
							for( uint64_t j = 0; j < opt.DIMPOW2; ++j ){
								uint64_t isamp = isamp0 * opt.DIMPOW2 + j;
								samples[iresl+1].push_back( SearchPoint(isamp) );
							}
						}
						if( 0 == samples[iresl+1].size() ){
							search_failed = true;
							std::cout << "search fail, no valid samples!" << std::endl;
							break;
						}
						samples[iresl].clear();

					}
					if( search_failed ) continue;
					std::cout << "full sort of final samples" << std::endl;
					__gnu_parallel::sort( samples.back().begin(), samples.back().end() );
				}
				if( search_failed ) continue;

				std::chrono::duration<double> elapsed_seconds_rif = std::chrono::high_resolution_clock::now()-start_rif;
				time_rif += elapsed_seconds_rif.count();

				std::cout << "total non-0 space size was approx " << float(non0_space_size)*1024.0*1024.0*1024.0 << " grid points" << std::endl;
				std::cout << "total search effort " << KMGT(total_search_effort) << std::endl;
				////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
				//////////////////////////////////////////////         HACK PACK           /////////////////////////////////////////////////////////////
				////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
		        std::chrono::time_point<std::chrono::high_resolution_clock> start_pack = std::chrono::high_resolution_clock::now();

				if( opt.hack_pack ){

					if( opt.use_scaffold_bounding_grids ){
						if( 0 == scene_minimal->template num_actors<SimpleAtom>(0) ){
							BOOST_FOREACH( SimpleAtom const & sa, target_simple_atoms )	scene_minimal->add_actor( 0, sa );
							runtime_assert( scene_minimal->template num_actors<SimpleAtom>(0) == target_simple_atoms.size() );
						}
					} else {
						// for final stage, use all scaffold atoms, not just CB ones
						runtime_assert( scene_minimal->clear_actors<SimpleAtom>( 1 ) );
						runtime_assert( scene_minimal->template num_actors<SimpleAtom>(1) == 0 );
						BOOST_FOREACH( SimpleAtom const & sa, scaffold_simple_atoms_all ) scene_minimal->add_actor( 1, sa );
						runtime_assert( scene_minimal->template num_actors<SimpleAtom>(1) == scaffold_simple_atoms_all.size() );
						// these should be shallow copies in scene_pt
						// so editing scene_minimal will change all conformations
						runtime_assert( scene_pt.front()->template num_actors<SimpleAtom>(1) == scaffold_simple_atoms_all.size() );
					}

					// if( scene_minimal->template num_actors<SimpleAtom>(0) == 0 ){
					// 	for(int i = 0; i < 10; ++i) std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!! hackpack add sterics back !!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
					// 	BOOST_FOREACH( SimpleAtom const & sa, target_simple_atoms )	scene_minimal->add_actor( 0, sa );
					// }
					// runtime_assert( scene_minimal->template num_actors<SimpleAtom>(0) == target_simple_atoms.size() );

				    std::chrono::time_point<std::chrono::high_resolution_clock> start, end;
			        start = std::chrono::high_resolution_clock::now();

					size_t n_packsamp = 0;
					for( n_packsamp; n_packsamp < samples.back().size(); ++n_packsamp ){
						if( samples.back()[n_packsamp].score > 0 ) break;
					}
					int const config = RESLS.size()-1;
					npack = std::min( n_packsamp, (size_t)(total_search_effort *
						( opt.hack_pack_frac / (packopts.pack_n_iters*packopts.pack_iter_mult)) ) );
					//////////////
					// npack = 1;
					/////////////
					packed_results.resize( npack );
					print_header( "hack-packing top " + KMGT(npack) );
					std::cout << "packing options: " << packopts << std::endl;
					std::cout << "packing w/rif rofts ";
					int64_t const out_interval = std::max<int64_t>(1,npack/100);
					std::exception_ptr exception = nullptr;
					#ifdef USE_OPENMP
					#pragma omp parallel for schedule(dynamic,64)
					#endif
					for( int ipack = 0; ipack < npack; ++ipack ){
						if( exception ) continue;
						try {
							if( ipack%out_interval==0 ){ cout << '*'; cout.flush();	}
							uint64_t const isamp = samples.back()[ipack].index;
							if( samples.back()[ipack].score > opt.global_score_cut ) continue;
							packed_results[ ipack ].index = isamp;
							packed_results[ ipack ].prepack_rank = ipack;
							ScenePtr tscene( scene_pt[omp_get_thread_num()] );
							director->set_scene( isamp, RESLS.size()-1, *tscene );
							packed_results[ ipack ].score = packing_objective->score_with_rotamers( *tscene, packed_results[ ipack ].rotamers() );
						} catch( std::exception const & ex ) {
							#ifdef USE_OPENMP
							#pragma omp critical
							#endif
							exception = std::current_exception();
						}
					}
					if( exception ) std::rethrow_exception(exception);
			        end = std::chrono::high_resolution_clock::now();

					std::cout << std::endl;
					std::cout << "full sort of packed samples" << std::endl;
					__gnu_parallel::sort( packed_results.begin(), packed_results.end() );

					std::chrono::duration<double> elapsed_seconds_pack = end-start;
					std::cout << "packing rate: " << (double)npack/elapsed_seconds_pack.count()                   << " iface packs per second" << std::endl;
					std::cout << "packing rate: " << (double)npack/elapsed_seconds_pack.count()/omp_max_threads() << " iface packs per second per thread" << std::endl;

				} else {
					packed_results.resize( samples.back().size() );
					#ifdef USE_OPENMP
					#pragma omp parallel for schedule(dynamic,1024)
					#endif
					for( int ipack = 0; ipack < packed_results.size(); ++ipack ){
						packed_results[ipack].score = samples.back()[ipack].score;
						packed_results[ipack].index = samples.back()[ipack].index;
					}
				}

				std::chrono::duration<double> elapsed_seconds_pack = std::chrono::high_resolution_clock::now()-start_pack;
				time_pck += elapsed_seconds_pack.count();
			}






			bool const do_rosetta_score = opt.rosetta_score_fraction > 0 || opt.rosetta_score_then_min_below_thresh > -9e8 || opt.rosetta_score_at_least > 0;

			if( do_rosetta_score && opt.hack_pack ){

				std::chrono::time_point<std::chrono::high_resolution_clock> start_rosetta = std::chrono::high_resolution_clock::now();

				int n_score_calculations = 0;
				int do_min = 2;
				if( opt.rosetta_min_fraction == 0.0 ) do_min = 1;

				std::vector<bool> is_scaffold_fixed_res(scaffold.size()+1,true);
				for(int designable : scaffold_res){
					is_scaffold_fixed_res[designable] = false;
				}

				for( int minimizing = 0; minimizing < do_min; ++minimizing ){

					std::chrono::duration<double> time_copy, time_score, time_min;

					//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
					if( minimizing ) print_header( "rosetta min and score" ); ////////////////////////////////////////////////////////////////////
					else         print_header( "rosetta score" ); ////////////////////////////////////////////////////////////////////////////
					//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

					std::vector<protocols::simple_moves::MinMoverOP> minmover_pt(omp_max_threads());
					std::vector<core::scoring::ScoreFunctionOP> scorefunc_pt(omp_max_threads());
					// std::vector<core::pose::Pose> both_full_per_thread(omp_max_threads());
					std::vector<core::pose::Pose> both_per_thread     (omp_max_threads());
					std::vector<core::pose::Pose> target_pt           (omp_max_threads());
					std::vector<core::pose::Pose> work_pose_pt        (omp_max_threads());
					for( int i = 0; i < omp_max_threads(); ++i){
						// both_full_per_thread[i] = both_full_pose;
						if( opt.replace_orig_scaffold_res ){
							both_per_thread[i] = both_full_pose;
						} else {
							both_per_thread[i] = both_pose;
						}
						scorefunc_pt[i] = core::scoring::ScoreFunctionFactory::create_score_function(opt.rosetta_soft_score);
						if( minimizing ){
							if( opt.rosetta_hard_min ){
								scorefunc_pt[i] = core::scoring::ScoreFunctionFactory::create_score_function(opt.rosetta_hard_score);
							} else {
								scorefunc_pt[i] = core::scoring::ScoreFunctionFactory::create_score_function(opt.rosetta_soft_score);
							}
						} else if( do_min==2 ){
							// not minimizing, but will do minimization
							scorefunc_pt[i] = core::scoring::ScoreFunctionFactory::create_score_function(opt.rosetta_soft_score);
							if( !opt.rosetta_hard_min ){
								scorefunc_pt[i]->set_weight( core::scoring::fa_rep, scorefunc_pt[i]->get_weight(core::scoring::fa_rep)*0.7 );
								scorefunc_pt[i]->set_weight( core::scoring::fa_dun, scorefunc_pt[i]->get_weight(core::scoring::fa_dun)*0.7 );
							}
						} else {
							// not minimizing at all, score pass only
							scorefunc_pt[i] = core::scoring::ScoreFunctionFactory::create_score_function(opt.rosetta_soft_score);
							if( !opt.rosetta_hard_min ){
								scorefunc_pt[i]->set_weight( core::scoring::fa_rep, scorefunc_pt[i]->get_weight(core::scoring::fa_rep)*1.0 );
								scorefunc_pt[i]->set_weight( core::scoring::fa_dun, scorefunc_pt[i]->get_weight(core::scoring::fa_dun)*1.0 );
							}
						}
						if( target.size() == 1 ){
							// assume this is a ligand, so hbonding is important
							scorefunc_pt[i]->set_weight( core::scoring::fa_elec    , scorefunc_pt[i]->get_weight(core::scoring::fa_elec    )*2.0 );
							scorefunc_pt[i]->set_weight( core::scoring::hbond_sc   , scorefunc_pt[i]->get_weight(core::scoring::hbond_sc   )*2.0 );
							scorefunc_pt[i]->set_weight( core::scoring::hbond_bb_sc, scorefunc_pt[i]->get_weight(core::scoring::hbond_bb_sc)*2.0 );
						} else {
							scorefunc_pt[i]->set_weight( core::scoring::fa_elec    , scorefunc_pt[i]->get_weight(core::scoring::fa_elec    )*1.0 );
							scorefunc_pt[i]->set_weight( core::scoring::hbond_sc   , scorefunc_pt[i]->get_weight(core::scoring::hbond_sc   )*1.0 );
							scorefunc_pt[i]->set_weight( core::scoring::hbond_bb_sc, scorefunc_pt[i]->get_weight(core::scoring::hbond_bb_sc)*1.0 );
						}

                        // score one pose single threaded maybe helps lkball issue
                        scorefunc_pt[i]->score(both_per_thread[i]);

						// scorefunc_pt[i]->set_weight( core::scoring::fa_rep, scorefunc_pt[i]->get_weight(core::scoring::fa_rep)*0.67 );
						// scorefunc_pt[i]->set_weight( core::scoring::fa_dun, scorefunc_pt[i]->get_weight(core::scoring::fa_dun)*0.67 );
						// core::scoring::methods::EnergyMethodOptions opts = scorefunc_pt[i]->energy_method_options();
						// core::scoring::hbonds::HBondOptions hopts = opts.hbond_options();
						// hopts.use_hb_env_dep( false );
						// opts.hbond_options( hopts );
						// scorefunc_pt[i]->set_energy_method_options( opts );
						core::kinematics::MoveMapOP movemap = core::kinematics::MoveMapOP( new core::kinematics::MoveMap() );
						movemap->set_chi(true);
						movemap->set_jump(true);
						for(int ir = 1; ir <= both_full_pose.size(); ++ir){
							bool is_scaffold = ir <= scaffold.size();
							if( is_scaffold ) movemap->set_bb(ir, opt.rosetta_min_allbb || opt.rosetta_min_scaffoldbb );
							else              movemap->set_bb(ir, opt.rosetta_min_allbb || opt.rosetta_min_targetbb );
							if( opt.rosetta_min_fix_target && !is_scaffold ){
								movemap->set_chi(ir,false);
							}
						}
						minmover_pt[i] = protocols::simple_moves::MinMoverOP(
							new protocols::simple_moves::MinMover( movemap, scorefunc_pt[i], "dfpmin_armijo_nonmonotone", 0.001, true ) );
					}

				    std::chrono::time_point<std::chrono::high_resolution_clock> startall = std::chrono::high_resolution_clock::now();

					size_t n_scormin = 0;
					if( minimizing ){
						// min take ~10x score time, so do on 1/10th of the scored
						n_scormin = n_score_calculations * opt.rosetta_min_fraction;
						n_scormin = std::ceil(1.0f*n_scormin/omp_max_threads()) * omp_max_threads();
					} else {
						// for scoring, use user cut
						n_scormin = opt.rosetta_score_fraction/40.0 * total_search_effort;
						if( opt.rosetta_score_then_min_below_thresh > -9e8 ){
							for( n_scormin=0; n_scormin < packed_results.size(); ++n_scormin ){
								if( packed_results[n_scormin].score > opt.rosetta_score_then_min_below_thresh )
									break;
							}
						}
						n_scormin = std::min<int>( std::max<int>( n_scormin, opt.rosetta_score_at_least ), opt.rosetta_score_at_most );
						n_scormin = std::min<int>( n_scormin, packed_results.size() );
						n_score_calculations = n_scormin;
					}
					packed_results.resize(n_scormin);
					int64_t const out_interval = std::max<int64_t>(1,n_scormin/50);
					if( minimizing) std::cout << "rosetta min on "   << KMGT(n_scormin) << ": ";
					else            std::cout << "rosetta score on " << KMGT(n_scormin) << ": ";
                    std::exception_ptr exception = nullptr;


					#ifdef USE_OPENMP
					#pragma omp parallel for schedule(dynamic,1)
					#endif
					for( int imin = 0; imin < n_scormin; ++imin )

                    {


                        try
    					{
    						if( imin%out_interval==0 ){ cout << '*'; cout.flush();	}

    						int const ithread = omp_get_thread_num();

    						director->set_scene( packed_results[imin].index, RESLS.size()-1, *scene_pt[ithread] );
    						EigenXform xposition1 = scene_pt[ithread]->position(1);
    						EigenXform xalignout = EigenXform::Identity();
    						if( opt.align_to_scaffold ) xalignout = xposition1.inverse();

    					    std::chrono::time_point<std::chrono::high_resolution_clock> start, end;
    				        start = std::chrono::high_resolution_clock::now();

    						// core::pose::PoseOP pose_to_min_ptr = core::pose::PoseOP(new core::pose::Pose);
    						core::pose::Pose & pose_to_min( work_pose_pt[ithread] );
    						pose_to_min = both_per_thread[ithread];
    						xform_pose( pose_to_min, eigen2xyz(xalignout)            , scaffold.size()+1 , pose_to_min.size() );
    						xform_pose( pose_to_min, eigen2xyz(xalignout*xposition1) ,                      1 ,    scaffold.size() );

    						// place the rotamers
    						core::chemical::ResidueTypeSetCAP rts = core::chemical::ChemicalManager::get_instance()->residue_type_set("fa_standard");
    						std::vector<bool> is_rif_res(pose_to_min.size(),false);
    						for( int ipr = 0; ipr < packed_results[imin].numrots(); ++ipr ){
    							int ires = scaffres_l2g.at( packed_results[imin].rotamers().at(ipr).first );
    							int irot =                  packed_results[imin].rotamers().at(ipr).second;
    							core::conformation::ResidueOP newrsd = core::conformation::ResidueFactory::create_residue( rts.lock()->name_map(rot_index.resname(irot)) );
    							pose_to_min.replace_residue( ires+1, *newrsd, true );
    							is_rif_res[ires] = true;
    							for( int ichi = 0; ichi < rot_index.nchi(irot); ++ichi ){
    								pose_to_min.set_chi( ichi+1, ires+1, rot_index.chi( irot, ichi ) );
    							}
    						}

    						EigenXform Xtorifframe = xalignout.inverse();
    						// clash check existing scaffold rotamers
    						// #pragma omp critical
    						// {
    						// 	pose_to_min.dump_pdb("test0.pdb");
    						// 	xform_pose(pose_to_min, eigen2xyz(Xtorifframe));
    						// 	pose_to_min.dump_pdb("test1.pdb");
    						// 	utility_exit_with_message("foo");
    						// }

    						std::vector<int> replaced_scaffold_res, rifres;
    						auto alaop = core::conformation::ResidueFactory::create_residue( rts.lock()->name_map("ALA" ) );

    						// pose_to_min.dump_pdb("test_rep_scaff_rots_"+str(imin)+"_before.pdb");

    						std::vector<int> rifatypemap = get_rif_atype_map();
    						for( int ir = 1; ir <= scaffold.size(); ++ir){
    							auto const & ires = pose_to_min.residue(ir);
    							if( !ires.is_protein() ) continue;
    							if( ires.aa()==core::chemical::aa_gly ||
    								ires.aa()==core::chemical::aa_ala ||
    								ires.aa()==core::chemical::aa_pro ) continue;
    							if(is_rif_res[ir-1]){
    								rifres.push_back(ir);
    								continue;
    							}
    							if(is_scaffold_fixed_res[ir]) continue;
    							bool ir_clash = false;

    							// check against target_field_by_atype
    							float evtarget = 0.0;
    							for( int ia = 6; ia <= ires.nheavyatoms(); ++ia){
    								auto const & ixyz = ires.xyz(ia);

    								Eigen::Vector3f satm;
    								for(int k = 0; k < 3; ++k) satm[k] = ixyz[k];
    								satm = Xtorifframe * satm;
    								int const irifatype = rifatypemap[ires.atom_type_index(ia)];
    								evtarget += target_field_by_atype.at(irifatype)->at(satm);
    							}
    							if( evtarget > 3.0f ) ir_clash = true;

    							// check against other rif res
    							for( int ipr = 0; ipr < packed_results[imin].numrots(); ++ipr ){
    								int jr = 1+scaffres_l2g.at( packed_results[imin].rotamers().at(ipr).first );
    								auto const & jres = pose_to_min.residue(jr);
    								// should do rsd nbr check... but speed not critical here ATM...
    								for( int ia = 6; ia <= ires.nheavyatoms(); ++ia){
    									auto const & ixyz = ires.xyz(ia);
    									for( int ja = 6; ja <= jres.nheavyatoms(); ++ja){
    										auto const & jxyz = jres.xyz(ja);
    										if( ixyz.distance_squared(jxyz) < 9.0 ){
    											ir_clash = true;
    										}
    									}
    								}
    							}
    							if(ir_clash){
    								pose_to_min.replace_residue(ir, *alaop, true);
    								replaced_scaffold_res.push_back(ir);
    							}
    						}

    						// pose_to_min.dump_pdb("test_rep_scaff_rots_"+str(imin)+"_after.pdb");
    						// if(imin >= 9) utility_exit_with_message("testing...aorsenoi");


    						end = std::chrono::high_resolution_clock::now();
    						std::chrono::duration<double> elapsed_seconds_copypose = end-start;
    						#pragma omp critical
    						time_copy += elapsed_seconds_copypose;
    						// pose_to_min.dump_pdb("test_pre.pdb");

    						if( minimizing ){
    							// std::cout << "MIN!" << std::endl;
    					        // start = std::chrono::high_resolution_clock::now();
    					        // #pragma omp critical
    					        // {
    						       //  pose_to_min.dump_pdb("min_"+str(imin)+"_before.pdb");
    								minmover_pt[ithread]->apply( pose_to_min );
    						    //     pose_to_min.dump_pdb("min_"+str(imin)+"_after.pdb");
    						    //     utility_exit_with_message("test_min");
    						    // }
    					        end = std::chrono::high_resolution_clock::now();
    							std::chrono::duration<double> elapsed_seconds_min = end-start;
    							#pragma omp critical
    							time_min += elapsed_seconds_min;
    						} else {
    							// std::cout << "SCORE!" << std::endl;
    					        start = std::chrono::high_resolution_clock::now();
    							scorefunc_pt[ithread]->score( pose_to_min );
    					        end = std::chrono::high_resolution_clock::now();
    							std::chrono::duration<double> elapsed_seconds_score = end-start;
    							#pragma omp critical
    							time_score += elapsed_seconds_score;
    						}

    						// if( target.size() == 1 ){
    						// 	// ligand!
    						// 	float total_lj_neg  = scorefunc_pt[ithread]->get_weight(core::scoring::fa_atr) * pose_to_min.energies().total_energies()[core::scoring::fa_atr];
    						// 	      total_lj_neg += scorefunc_pt[ithread]->get_weight(core::scoring::fa_rep) * pose_to_min.energies().total_energies()[core::scoring::fa_rep];
    						// 	      total_lj_neg = std::max(0.0f,total_lj_neg);
    						// 	packed_results[imin].score  = 1.00*total_lj_neg;
    						// 	packed_results[imin].score += 1.00*pose_to_min.energies().total_energies()[core::scoring::hbond_sc]; // last res is ligand
    						// 	packed_results[imin].score += 1.00*pose_to_min.energies().residue_total_energy(pose_to_min.size());
    						// 	for( int ipr = 0; ipr < packed_results[imin].numrots(); ++ipr ){
    						// 		int ires = scaffres_l2g.at( packed_results[imin].rotamers().at(ipr).first );
    						// 		packed_results[imin].score += 0.5*pose_to_min.energies().residue_total_energy(ires);
    						// 	}
    						// } else {
    							// not ligand! compute the fixed-everything ddg
    						if( opt.rosetta_score_total ){
    							packed_results[imin].score = pose_to_min.energies().total_energy();
    						} else {
    							double rosetta_score = 0.0;
    							auto const & weights = pose_to_min.energies().weights();
    							if( !opt.rosetta_score_ddg_only ){
    								for( int ir = 1; ir <= scaffold.size(); ++ir ){
    									if( is_rif_res[ir-1] ){
    										rosetta_score += pose_to_min.energies().onebody_energies(ir).dot(weights);
    									}
    								}
    							}
    							if( !opt.rosetta_score_ddg_only && target.size()==1 ){
    								// is ligand, add it's internal energy
    								rosetta_score += pose_to_min.energies().onebody_energies(pose_to_min.size()).dot(weights);
    							}
    							auto const & egraph = pose_to_min.energies().energy_graph();
    							for(int ir = 1; ir <= egraph.num_nodes(); ++ir){
    								for ( utility::graph::Graph::EdgeListConstIter
    										iru  = egraph.get_node(ir)->const_upper_edge_list_begin(),
    										irue = egraph.get_node(ir)->const_upper_edge_list_end();
    										iru != irue; ++iru
    								){
    									EnergyEdge const & edge( static_cast< EnergyEdge const & > (**iru) );
    									int jr = edge.get_second_node_ind();

    									// this is DDG
    									if( ir <= scaffold.size() && jr > scaffold.size() ){
    										// ir in scaff, jr in target
    										rosetta_score += edge.dot(weights);
    									}
    									if( !opt.rosetta_score_ddg_only && jr <= scaffold.size() ){
    										// ir & jr in scaffold
    										if( is_rif_res[ir-1] || is_rif_res[jr-1] ){
    											double const edgescore = edge.dot(weights);
    											if( edgescore > 0.0 ){
    												// always assess full score for bad interactions
    												rosetta_score += edgescore;
    											} else if( is_rif_res[ir-1] && is_rif_res[jr-1] ){
    												// both rif residues
    												rosetta_score += opt.rosetta_score_rifres_rifres_weight * edgescore;
    												// bonus for hbonds between rif residues
    												rosetta_score += edge[core::scoring::hbond_sc];
    											} else {
    												// rest: one rif res, one other scaff res
    												rosetta_score += opt.rosetta_score_rifres_scaffold_weight * edgescore;
    											}
    										} else {
    											// scaffold / scaffold ignored
    										}
    									}
    								}
    							}
    							packed_results[imin].score = rosetta_score;
    							// #pragma omp critical
    							// std::cout << rosetta_score << std::endl;
    						}


    						if( (minimizing+1 == do_min)	 && packed_results[imin].score < opt.rosetta_score_cut ){
    							packed_results[imin].pose_ = core::pose::PoseOP( new core::pose::Pose(pose_to_min) );
    							for(int ir : rifres){
    								packed_results[imin].pose_->pdb_info()->add_reslabel(ir, "RIFRES" );
    							}
    							for(int ir : replaced_scaffold_res){
    								packed_results[imin].pose_->pdb_info()->add_reslabel(ir, "PRUNED" );
    							}
    						}

    						// #pragma omp critical
    						// pose_to_min.energies().show(cout,pose_to_min.size());
    						// pose_to_min.dump_pdb("test_post.pdb");
    						// #pragma omp critical
    						// std::cout << imin << " prescore: " << e_pre_min << " minscore: " << pose_to_min.energies().total_energy() << std::endl;
    						// utility_exit_with_message("testing");
    					} catch(...) {
                            #pragma omp critical
                            exception = std::current_exception();
                        }

                    } // end of OMP loop
                    if( exception ) std::rethrow_exception(exception);


					cout << endl;
					__gnu_parallel::sort( packed_results.begin(), packed_results.end() );
					{
						size_t n_scormin = 0;
						for( n_scormin; n_scormin < packed_results.size(); ++n_scormin ){
							if( minimizing && packed_results[n_scormin].score > opt.rosetta_score_cut ) break;
						}
						packed_results.resize(n_scormin);
					}

				    std::chrono::time_point<std::chrono::high_resolution_clock> stopall = std::chrono::high_resolution_clock::now();
					std::chrono::duration<double> elap_sec = stopall - startall;

					if( minimizing ){
						std::cout << "total pose copy time: " << time_copy.count() << "s total min time: " << time_min.count() << "s" << " walltime total " << elap_sec.count() << std::endl;
						std::cout << "min rate: "   << (double)n_scormin / elap_sec.count()                     << " sc/rb minimizations per second" << std::endl;
						std::cout << "min rate: "   << (double)n_scormin / elap_sec.count() / omp_max_threads() << " sc/rb minimizations per second per thread" << std::endl;
					} else {
						std::cout << "total pose copy time: " << time_copy.count() << "s total score time: " << time_score.count() << "s" << " walltime total " << elap_sec.count() << std::endl;
						std::cout << "score rate: " << (double)n_scormin / elap_sec.count()                     << " rosetta scores per second" << std::endl;
						std::cout << "score rate: " << (double)n_scormin / elap_sec.count() / omp_max_threads() << " rosetta scores per second per thread" << std::endl;
					}
				}

				std::chrono::duration<double> elapsed_seconds_rosetta = std::chrono::high_resolution_clock::now()-start_rosetta;
				time_ros += elapsed_seconds_rosetta.count();

			}







			//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			print_header( "compile and filter results" ); ///////////////////////////////////////////////////////////////////////////
			//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			

			std::vector< RifDockResult > selected_results, allresults;
			{
				CompileAndFilterResultsData<DirectorBase> data {
					opt, 
					packed_results, 
					RESLS, 
					scene_pt, 
					director, 
					redundancy_filter_rg, 
					scaffold_center, 
					dump_lock,
					objectives, 
					scaffold_perturb
				};

				compile_and_filter_results( selected_results, allresults, data );
			}


			std::cout << "allresults.size(): " << allresults.size() << " selected_results.size(): " << selected_results.size() << std::endl;

			////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			print_header( "timing info" ); //////////////////////////////////////////////////////////////////////////////////////////////
			///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

			std::cout<<"total RIF     time: "<<KMGT(time_rif)<<" fraction: "<<time_rif/(time_rif+time_pck+time_ros)<<std::endl;
			std::cout<<"total Pack    time: "<<KMGT(time_pck)<<" fraction: "<<time_pck/(time_rif+time_pck+time_ros)<<std::endl;
			std::cout<<"total Rosetta time: "<<KMGT(time_ros)<<" fraction: "<<time_ros/(time_rif+time_pck+time_ros)<<std::endl;			

			////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			print_header( "output results" ); //////////////////////////////////////////////////////////////////////////////////////////////
			///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


			BOOST_FOREACH( RifDockResult const & r, allresults ){
				; // nothing with all results ATM
			}

			{
				OutputResultsData<DirectorBase> data { opt, 
					RESLS, 
					director, 
					selected_results,
					scafftag,
					npack,
					dokout,
					scene_full,
					scene_minimal,
					scaffres_g2l,
					scaffres_l2g,
					rif_ptrs,
					scaffold_onebody_glob0,
					rot_index,
					scaffold,
					both_pose,
					both_full_pose,
					scaffold_only_pose,
					scaffold_only_full_pose
				};
				output_results(data);
			}
			

		} catch( std::exception const & ex ) {
			std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
			std::cout << "error (below) on scaffold " << scaff_fname << " (will continue with others, if any)" << std::endl;
			std::cout << ex.what() << std::endl;
			std::cout << "scene residue numering (may help debug):" << std::endl;
			for( int i = 1; i <= scaffold_res.size(); ++i ){
				std::cout << "scene res numbering: " << i-1 << " " << scaffold_sequence_glob0.at(scaffold_res[i]-1) << " pose number: " << scaffold_res[i] << std::endl;
			}
			std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
		} catch ( ... ) {
			std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
			std::cout << "unknown error on scaffold " << scaff_fname << ", will continue with others, if any." << std::endl;
			std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
		}


	} // end scaffold loop


	dokout.close();






	#ifdef USE_OPENMP
		omp_destroy_lock( &cout_lock );
		omp_destroy_lock( &dump_lock );
	#endif

	std::cout << "rif_dock_test_DONE" << std::endl;

	return 0;
 }
