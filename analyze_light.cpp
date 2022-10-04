// main driver for scintillation toy mc simulation code
#include<string>
#include<iostream>
#include<fstream>
#include<chrono>
#include <sstream>
#include <vector>
#include <algorithm>
#include "TH1.h"
#include "TRandom.h"
#include "TVector3.h"

#include "data_output.h"
#include "semi_analytic_hits.h"
#include "time_parameterisation.h"
#include "utility_functions.h"
#include "radiological_parameters.h"

// include parameter file
#include "simulation_parameters.h"
using namespace std;

bool debug = false;
bool diffusion = true;

double Area2Radius(double area) {
  return sqrt(area / TMath::Pi());
}

int main(int argc, char* argv[]){

	if(argc < 4){
		cout << "Usage: " << argv[0] << " <G4_input_file> <SiPM_placement_file> <PixelSize(cm)> <OutputFile> (<inlcude_input>)" << endl;
		return 1;
	}

	bool include_input = true;
	if(argc>5){
		include_input  = stoi(argv[5]);
	}
	else{
		include_input = true;
	}

	cout << endl;
	cout << endl;
	cout << "Running with G4 file: " << argv[1] << endl;
	cout << "Running with SiPM file: " << argv[2] << endl;
	cout << "Running with a Pixel Size of : " << stod(argv[3]) << " cm" << endl;
	cout << "Output File: " << argv[4];
	if(include_input){
		cout << " - Including input tree event_tree in output" << endl;
	}
	else{
		cout << " - Not Including input tree event_tree in output" << endl;
	}
	cout << endl;

	gRandom->SetSeed(0);

	// -------- Initialise semi-analytic hits class ---------
	semi_analytic_hits hits_model;
	hits_model.setPixelSize(stod(argv[3]),stod(argv[3]));
	double PixSize = stod(argv[3]);

	// -------- Initialise timing parametrisation class ---------
	time_parameterisation times_model(parameters::timing_discretisation_step_size);

	// -------- Initialise utility/energy spectrum class ---------
	utility_functions utility;
	utility.initalise_scintillation_functions_argon(parameters::t_singlet, parameters::t_triplet, parameters::singlet_fraction_electron, parameters::triplet_fraction_electron,
        parameters::singlet_fraction_alpha, parameters::triplet_fraction_alpha, parameters::scint_time_window);
        utility.initalise_scintillation_functions_xenon(parameters::t_singlet_Xe, parameters::t_triplet_Xe, parameters::singlet_fraction_Xe, parameters::triplet_fraction_Xe, parameters::scint_time_window);

	// ------- Read photon detector positions and types --------
	std::vector<std::vector<int>> opdet_type;
	std::vector<int> opdet_direction;
	std::vector<std::vector<double>> opdet_position;

       	std::cout << "Loading Photon Detector positions..." << std::endl;
        std::ifstream detector_positions_file;
	//detector_positions_file.open("optical_detectors_dune1x2x6.txt");
        //detector_positions_file.open("optical_detectors_QPIX4mm.txt");
        detector_positions_file.open(argv[2]);
        if(detector_positions_file.is_open()) std::cout << "File opened successfully" << std::endl;
        else {std::cout << "File not found." << std::endl; exit(1);}
        while(!detector_positions_file.eof()) {
		int num_opdet, type_opdet, direction; double x_opdet, y_opdet, z_opdet;
		if(detector_positions_file >> num_opdet >> x_opdet >> y_opdet >> z_opdet >> type_opdet >> direction) {
		    std::vector<int> type({num_opdet, type_opdet});
		    std::vector<double> position({x_opdet, y_opdet, z_opdet});
		    opdet_type.push_back(type);
		    opdet_position.push_back(position);
		    opdet_direction.push_back(direction);
		}
		else{ break; }
        }
	detector_positions_file.close();
	int number_opdets = opdet_type.size();
	std::cout << "Positions Loaded: " << number_opdets << " optical detectors." << std::endl << std::endl;

	// ------- Read G4 simulation data --------
	// READ IN THE G4 SIMULATION  - CURRENTLY HARDCODED
	char* G4InputFileName = argv[1];
	TFile * G4InputFile = new TFile(G4InputFileName);

	TTree *G4InputTree = (TTree*)G4InputFile->Get("event_tree");

	double energy_deposit;
	vector <double> *hit_start_x = nullptr;
	vector <double> *hit_start_y = nullptr;
	vector <double> *hit_start_z = nullptr;
	vector <double> *hit_start_t = nullptr;
	vector <double> *hit_energy_deposit = nullptr;
	vector <double> *hit_length = nullptr;
  	vector<double> *particle_pdg_code = nullptr;
  	vector<double> *hit_track_id= nullptr;
	double pixel_size;


	G4InputTree->SetBranchAddress("energy_deposit", &energy_deposit);
	G4InputTree->SetBranchAddress("hit_start_x", &hit_start_x);
	G4InputTree->SetBranchAddress("hit_start_y", &hit_start_y);
	G4InputTree->SetBranchAddress("hit_start_z", &hit_start_z);
	G4InputTree->SetBranchAddress("hit_length", &hit_length);
	G4InputTree->SetBranchAddress("hit_start_t", &hit_start_t);
	G4InputTree->SetBranchAddress("hit_energy_deposit", &hit_energy_deposit);
	G4InputTree->SetBranchAddress("hit_length", &hit_length);
  	G4InputTree->SetBranchAddress("hit_track_id", &hit_track_id);
  	G4InputTree->SetBranchAddress("particle_pdg_code", &particle_pdg_code);

	int NEventsToLoopOver = G4InputTree->GetEntries(); // 100000
	data_output output_file(argv[4], include_input, parameters::include_timings, parameters::include_reflected, G4InputFileName );
        //char *second = "output.root";
	//data_output output_file(second, parameters::include_timings, parameters::include_reflected, G4OutputFileName );
	double SiPM_QE = 0.4;
	double total_QE = SiPM_QE;
	double drift_velocity = 0.16; // cm/us
	double drift_long = 6.627; //cm^2/s
	double drift_trans = 13.237; //cm^2/s

	for (int EventIt=0; EventIt < NEventsToLoopOver; EventIt++)
	{
		//if(EventIt!=1894) continue;
		std::cout << " --- > Event: " << EventIt << std::endl;
		G4InputTree->GetEntry(EventIt);

		int max_events = hit_start_x->size();

		// Vector inlcuding all the hit positions in (x,y,z)
		std::vector<std::vector<double>> position_list(max_events, std::vector<double>(3,-999.9));

		// Vector including the number of photons per SiPM
		vector<int> num_VUV_array;

		// Vector including the current hits x,y,z positoin
		vector<TVector3> ScintPoint_array;

		// Vector of vectors.
		// [ SiPM1[t0, t1, t2,t3,...], SiPM2[t0, t1, t2,t3,...], SiPM3[t0, t1, t2,t3,...], ...]
		// For each SiPM there should be a vector. This vector will contain the time of each photon.
		vector<vector<double>> total_time_vuv_array ;//(number_opdets, vector<double>);
		total_time_vuv_array.clear();
		// Vecotr of vectors for the charge readout
		vector<vector<double>> total_time_charge;//(number_opdets, vector<double>);
		total_time_charge.clear();

		vector<vector<double>> op_channel_pos(number_opdets, vector<double>(3,0.0));
		std::cout << "Number of optical detectors: " << number_opdets << std::endl;

		// Determining the number of photons due to each hit
		std::vector<double> lightyield(hit_start_x->size());
		std::vector<double> chargeyield(hit_start_x->size());
		std::vector<double> numPhotons(hit_start_x->size());
		std::vector<double> numElectrons(hit_start_x->size());
		std::vector<std::vector<TVector3>> electronStartingPoints;

		std::cout << "Preparing the Energy Depositions" << std::endl;

		for (int i=0; i < hit_start_x->size(); i++){
			        std::vector<TVector3> electronStartingPoints_i;
				lightyield[i] = hits_model.LArQL(hit_energy_deposit->at(i), hit_length->at(i), 0.5);
				chargeyield[i] = hits_model.LArQQ(hit_energy_deposit->at(i), hit_length->at(i), 0.5);
				numPhotons[i] = lightyield[i] * hit_energy_deposit->at(i);
				numElectrons[i] = chargeyield[i] * hit_energy_deposit->at(i);
				double z_pos = hit_start_z->at(i);
				double x_pos = hit_start_x->at(i);
				double y_pos = hit_start_y->at(i);
			        double drift_time = z_pos/drift_velocity;
		   		double area_trans = drift_trans * drift_time/1000*1000;
				double area_long = drift_long * drift_time/1000*1000;
				double area_phot_det = PixSize * PixSize;
				double rad_trans = Area2Radius(area_trans);
				double rad_long = Area2Radius(area_long);
				double x_pos_final = x_pos;
				double y_pos_final = y_pos;
				double z_pos_final = z_pos;
				for(int j=0; j<numElectrons[i]; j++){
					if(diffusion){
						double rand_angle = gRandom->Uniform(0,2*TMath::Pi());
						x_pos_final = x_pos + abs(gRandom->Gaus(0, rad_trans)) * cos(rand_angle);
						y_pos_final = y_pos + abs(gRandom->Gaus(0, rad_trans)) * sin(rand_angle);
						z_pos_final = z_pos + gRandom->Gaus(0,rad_long);
						drift_time = z_pos_final / drift_velocity ;
					}// End diffusion if
					electronStartingPoints_i.push_back(TVector3(x_pos_final, y_pos_final, z_pos_final));
				}
				electronStartingPoints.push_back(electronStartingPoints_i);
		}


		std::cout << "Simulating Charge" << std::endl;
		// Loop over the hits
		for(int op_channel = 0; op_channel < number_opdets; op_channel++) {
			std::vector<double> time_charge;
			TVector3 OpDetPoint(opdet_position[op_channel][0],opdet_position[op_channel][1],opdet_position[op_channel][2]);
			for(int HitIt = 0; HitIt < hit_start_x->size(); HitIt++){
				std::vector<TVector3> electronStartingPoints_i = electronStartingPoints[HitIt];
				for(int eIt = 0; eIt <numElectrons[HitIt] ; eIt++){
					TVector3 electronStartingPoint = electronStartingPoints_i[eIt];
					double x_pos = electronStartingPoint.X();
					double y_pos = electronStartingPoint.Y();
					double z_pos = electronStartingPoint.Z();
					double drift_time = z_pos/drift_velocity;
					if (abs(x_pos - OpDetPoint.X()) < 5 && abs(y_pos - OpDetPoint.Y()) < 5 ) {
						time_charge.push_back(drift_time);
					}
				}// end for of number electrons
			}// end for of hits
		total_time_charge.push_back(time_charge);
		}// end for of opdet

		// Get total number of electrons
		int total_num_electrons = 0;
		for(int i=0; i<numElectrons.size(); i++){
			total_num_electrons += numElectrons[i];
		}

		// Go through every SiPM
		std::cout << "Simulating Light" << std::endl;
		for(int op_channel = 0; op_channel < number_opdets; op_channel++) {
			/*
			if(op_channel % 10000 == 0)
				std::cout << "op_channel: " << op_channel
				   << " of " << number_opdets
			           << " (" << (double)op_channel*100./(double)number_opdets <<"%)\r";
			if(abs(op_channel - number_opdets)<2) std::cout << std::endl << " Finished Event " << EventIt << std::endl;
			*/

			// get optical detector type - rectangular or disk aperture
			int op_channel_type = opdet_type[op_channel][1];
			// get optical detector direction - (x, y, z)
			int op_direction = opdet_direction[op_channel];


			// get detection channel coordinates (in cm)
			TVector3 OpDetPoint(opdet_position[op_channel][0],opdet_position[op_channel][1],opdet_position[op_channel][2]);
			vector<double> op_det_pos = {OpDetPoint.X(), OpDetPoint.Y(), OpDetPoint.Z()};

			std::vector<double> total_time_vuv;

			// Go through every hit in the event
			int num_hits = hit_start_x->size();
			for (int i=0; i < num_hits; i++)
			{
				//cout << "Number of hits: " << op_channel<< endl;
		   		position_list[i][0] = hit_start_x->at(i);
		    		position_list[i][1] = hit_start_y->at(i);
		    		position_list[i][2] = hit_start_z->at(i);

				//cout << "Number of hits: " << op_channel<< endl;
		    		double time_hit = hit_start_t->at(i); // in ns
				time_hit*=0.001; // in us

				// Light yield for the hit currently looked at
				double light_yield = lightyield[i];

				unsigned int number_photons;
		    		number_photons = total_QE*light_yield*hit_energy_deposit->at(i);//utility.poisson(light_yield, gRandom->Uniform(1.), energy_deposit);
		  		assert(number_photons >= 0);

				int pdg = particle_pdg_code->at(hit_track_id->at(i)-1);
				if(pdg==22 || abs(pdg)==11 || abs(pdg)==12 || abs(pdg)==13 || abs(pdg)==14){
					// Gamma, e, nue, mu, numu
		    			double singlet_fraction = parameters::singlet_fraction_electron;
		    			double triplet_fraction = parameters::triplet_fraction_electron;
				}
				else if(pdg == 2212 || pdg == 2112 || pdg == 1000020040){
					// Proton, neutron, alpha
		    			double singlet_fraction = parameters::singlet_fraction_alpha;
		    			double triplet_fraction = parameters::triplet_fraction_alpha;
				}
				else {
					/* std::cout << "Unknown particle type: " << pdg << std::endl; */
					/* std::cout << "Setting fractions to zero " << std::endl; */
					double singlet_fraction = parameters::singlet_fraction_alpha;
		    			double triplet_fraction = parameters::triplet_fraction_alpha;
				}



		    		// Setting the point of energy deposition
		    		TVector3 ScintPoint(position_list[i][0],position_list[i][1],position_list[i][2]);
					if(debug){
					cout << " ------------------------------------------ " << endl;
					cout << "ScintPoint: " << ScintPoint.X() << " " << ScintPoint.Y() << " " << ScintPoint.Z() << endl;
					cout << "OpDetPoint: " << OpDetPoint.X() << " " << OpDetPoint.Y() << " " << OpDetPoint.Z() << endl;
					}

				int num_VUV = hits_model.VUVHits(number_photons, ScintPoint, OpDetPoint, op_channel_type, 0, op_direction);
				//cout << "Hit: " << i << " Optical Channel: " << op_channel << " " << num_VUV << endl;
				if(num_VUV== 0) { continue; } // forces the next iteration

					// Calculate the angle between the scinitllation point and the optical detector
				double distance_to_pmt = (OpDetPoint-ScintPoint).Mag();
				double cosine = -999.0;

				// TODO: Correct and check
				if(op_direction == 1)  cosine = sqrt(pow(ScintPoint[0] - OpDetPoint[0],2)) / distance_to_pmt;
				else if(op_direction == 2)  cosine = sqrt(pow(ScintPoint[1] - OpDetPoint[1],2)) / distance_to_pmt;
				else if(op_direction == 3)  cosine = sqrt(pow(ScintPoint[2] - OpDetPoint[2],2)) / distance_to_pmt;
				else { std::cout << "Error: Optical detector direction not defined." << std::endl; }

				assert(cosine>=-1 && cosine<=1);

				double theta = acos(cosine)*180./3.14159;
				if(debug){
					cout << "Distance to PMT: " << distance_to_pmt << endl;
					cout << "Theta: " << theta << endl;
				}

				// Due to rounding errors, this can return a value like 90.000x01 which will throw an error in the angle_bin histogram.
				// Cap the angle to 90 degrees.
				if (theta >= 90){ theta=89.99999; }
				int angle_bin = theta/45;       // 45 deg bins


				// For each photon we will store its arival time in the SiPM currently being looped over.
				// Returns the transport time that the photon takes from the sicntillation point to the detector
				// Returns the time that the photon arrives at the detector in NANOSECONDS
				std::vector<double> transport_time_vuv = times_model.getVUVTime(distance_to_pmt, angle_bin, num_VUV);

				// For each photon get the time info
				for(auto& x: transport_time_vuv) {
					// emission time
					double emission_time;
					if(pdg==22 || abs(pdg)==11 || abs(pdg)==12 || abs(pdg)==13 || abs(pdg)==14){
						emission_time = utility.get_scintillation_time_electron()*1000000.0; // in us
					}
					else if(pdg == 2212 || pdg == 2112 || pdg >= 1000000000){
						emission_time = utility.get_scintillation_time_alpha()*1000000.0; // in us
					}
					else{
					        emission_time = utility.get_scintillation_time_electron()*1000000.0; // in us
					}
					double total_time = time_hit+(x*0.001 + emission_time + 2.5*0.001); // in microseconds // WLS 2.5 ns - constant offset
					total_time_vuv.push_back(total_time);
				}// End for transporttime
		    } // end of hit
		total_time_vuv_array.push_back(total_time_vuv);
		} // End of SiPM Loop
	output_file.add_data_till(total_time_vuv_array,total_time_charge , lightyield, chargeyield);
	lightyield.clear();
	chargeyield.clear();
	} // end event loop

     //Write to OUTPUT FILE
     output_file.write_output_file();
     sleep(2);
     std::cout << "Program finished." << std::endl;
}
