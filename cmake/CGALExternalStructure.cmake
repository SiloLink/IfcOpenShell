# Keep this dependency correction local to the build. CGAL 5.6.3 frees the
# owning Nef's point locator before rebuilding; 6.2.1 delays the delete but
# leaks the replacement when rebuilding throws. Neither failure may transfer
# a dangling locator back through Nef_polyhedron_3::delegate.
function(ifcopenshell_cgal_external_structure include_dir output_dir)
    set(header "CGAL/Convex_decomposition_3/External_structure_builder.h")
    set(source "${include_dir}/${header}")
    file(READ "${source}" original)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")

    set(early_delete [=[    SNC_point_locator* old_pl = pl;
    pl = pl->clone();
    sncpl.pl = pl;
    delete old_pl;
    SNC_external_structure C(*sncp,pl);
    C.clear_external_structure();
    C.build_external_structure();]=])
    set(late_delete [=[    SNC_point_locator* old_pl = pl;
    pl = pl->clone();
    sncpl.pl = pl;
    SNC_external_structure C(*sncp,pl);
    C.clear_external_structure();
    C.build_external_structure();
    delete old_pl;]=])
    set(safe_rebuild [=[    // delegate commits sncpl.pl only on normal return. Keep its current
    // owner alive until rebuilding succeeds and dispose of the clone on error.
    std::unique_ptr<SNC_point_locator> replacement(pl->clone());
    SNC_external_structure C(*sncp, replacement.get());
    C.clear_external_structure();
    C.build_external_structure();
    sncpl.pl = replacement.release();
    delete pl;]=])

    string(FIND "${original}" "${early_delete}" early_position)
    string(FIND "${original}" "${late_delete}" late_position)
    if(NOT early_position EQUAL -1)
        string(REPLACE "${early_delete}" "${safe_rebuild}" corrected "${original}")
    elseif(NOT late_position EQUAL -1)
        string(REPLACE "${late_delete}" "${safe_rebuild}" corrected "${original}")
    else()
        message(FATAL_ERROR
            "Unrecognized CGAL External_structure_builder ownership code in ${source}. "
            "Review exception safety before updating the CGAL dependency.")
    endif()
    string(REPLACE "#include <CGAL/license/Convex_decomposition_3.h>"
        "#include <CGAL/license/Convex_decomposition_3.h>\n#include <memory>"
        corrected "${corrected}")
    file(MAKE_DIRECTORY "${output_dir}/CGAL/Convex_decomposition_3")
    # configure_file avoids touching the generated header on unchanged builds.
    set(temporary "${output_dir}/External_structure_builder.h.tmp")
    file(WRITE "${temporary}" "${corrected}")
    configure_file("${temporary}" "${output_dir}/${header}" COPYONLY)
    file(REMOVE "${temporary}")
endfunction()
