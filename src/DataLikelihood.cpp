#include "DataLikelihood.h"
#include "multichoose.h"
#include "multipermute.h"


long double
probObservedAllelesGivenGenotype(
        Sample& sample,
        Genotype& genotype,
        double dependenceFactor,
        bool useMapQ,
        Bias& observationBias,
        bool standardGLs,
        vector<Allele>& genotypeAlleles,
        Contamination& contaminations,
        map<string, double>& freqs
    ) {

    int observationCount = sample.observationCount();
    vector<long double> alleleProbs = genotype.alleleProbabilities(observationBias);
    vector<int> observationCounts = genotype.alleleObservationCounts(sample);
    int countOut = 0;
    double countIn = 0;
    long double prodQout = 0;  // the probability that the reads not in the genotype are all wrong
    long double probObsGivenGt = 0;
    
    if (standardGLs) {
        for (Sample::iterator s = sample.begin(); s != sample.end(); ++s) {
            const string& base = s->first;
            if (!genotype.containsAllele(base)) {
                vector<Allele*>& alleles = s->second;
                if (useMapQ) {
                    for (vector<Allele*>::iterator a = alleles.begin(); a != alleles.end(); ++a) {
                        // take the lesser of mapping quality and base quality (in log space)
                        prodQout += max((*a)->lnquality, (*a)->lnmapQuality);
                    }
                } else {
                    for (vector<Allele*>::iterator a = alleles.begin(); a != alleles.end(); ++a) {
                        prodQout += (*a)->lnquality;
                    }
                }
                countOut += alleles.size();
            }
        }
    } else {
        // problem.
        // we should do this over all alleles
        // which have partial support or full support.
        // this is only over
        vector<Allele*> emptyA;
        vector<Allele*> emptyB;
        for (set<string>::iterator c = sample.supportedAlleles.begin();
             c != sample.supportedAlleles.end(); ++c) {

            vector<Allele*>* alleles = &emptyA;
            Sample::iterator si = sample.find(*c);
            if (si != sample.end()) alleles = &si->second;

            vector<Allele*>* partials = &emptyB;
            map<string, vector<Allele*> >::iterator pi = sample.partialSupport.find(*c);
            if (pi != sample.partialSupport.end()) partials = &pi->second;

            bool onPartials = false;
            vector<Allele*>::iterator a = alleles->begin();
            bool hasPartials = !partials->empty();
            for ( ; (!hasPartials && a != alleles->end()) || a != partials->end(); ++a) {
                if (a == alleles->end()) {
                    if (hasPartials) {
                        a = partials->begin();
                        onPartials = true;
                    } else {
                        break;
                    }
                }
                Allele& obs = **a;
                long double probi = 0;
                ContaminationEstimate& contamination = contaminations.of(obs.readGroupID);
                double scale = 1;
                // note that this will underflow if we have mapping quality = 0
                // we guard against this externally, by ignoring such alignments (quality has to be > MQL0)
                long double qual = (1 - exp(obs.lnquality)) * (1 - exp(obs.lnmapQuality));
                if (onPartials) {
                    map<Allele*, set<Allele*> >::iterator r = sample.reversePartials.find(*a);
                    if (r != sample.reversePartials.end()) {
                        if (sample.reversePartials[*a].empty()) {
                            cerr << "partial " << *a << " has empty reverse" << endl;
                            exit(1);
                        }
                        //cerr << "partial " << *a << " supports potentially " << sample.reversePartials[*a].size() << " alleles : ";
                        //for (set<Allele*>::iterator m = sample.reversePartials[*a].begin();
                        //     m != sample.reversePartials[*a].end(); ++m) cerr << **m << " ";
                        //cerr << endl;
                        scale = (double)1/(double)sample.reversePartials[*a].size();
                    }
                }

                // TODO add partial obs, now that we have them recorded
                // how does this work?
                // each partial obs is recorded as supporting, but with observation probability scaled by the number of possible haplotypes it supports
                bool isInGenotype = false;

                for (vector<Allele>::iterator b = genotypeAlleles.begin(); b != genotypeAlleles.end(); ++b) {
                    Allele& allele = *b;
                    const string& base = b->currentBase;

                    long double q;
                    if (obs.currentBase == base
                        || (onPartials && sample.observationSupports(*a, &*b))) {
                        isInGenotype = true;
                        q = qual;
                    } else {
                        q = 1 - qual;
                    }

                    if (onPartials) {
                        q *= scale; // distribute partial support evenly across supported haplotypes
                    }

                    double asampl = genotype.alleleSamplingProb(base);
                    //double freq = freqs[base];

                    if (asampl == 0) {
                        // scale by frequency of (this) possibly contaminating allele
                        asampl = contamination.probRefGivenHomAlt;
                    } else if (asampl == 1) {
                        // scale by frequency of (other) possibly contaminating alleles
                        asampl = 1 - contamination.probRefGivenHomAlt;
                    } else {
                        // to deal with polyploids
                        // note that this reduces to 1 for diploid heterozygotes
                        double scale = asampl / 0.5;
                        // this term captures reference bias
                        if (allele.isReference()) {
                            asampl = scale * contamination.probRefGivenHet;
                        } else {
                            asampl = 1 - (scale * contamination.probRefGivenHet);
                        }
                    }

                    probi += asampl * q;

                }

                if (isInGenotype) {
                    countIn += scale;
                }

                // bound to (0,1]
                if (probi > 0) {
                    long double lnprobi = log(min(probi, (long double) 1.0));
                    probObsGivenGt += lnprobi;
                }
            }
        }
    }

    // read dependence factor, asymptotically downgrade quality values of
    // successive reads to dependenceFactor * quality
    if (standardGLs) {
        if (countOut > 1) {
            prodQout *= (1 + (countOut - 1) * dependenceFactor) / countOut;
        }

        if (sum(observationCounts) == 0) {
            return prodQout;
        } else {
            return prodQout + multinomialSamplingProbLn(alleleProbs, observationCounts);
        }
    } else {
        // read dependence factor, but inverted to deal with the new GL implementation
        if (countIn > 1) {
            probObsGivenGt *= (1 + (countIn - 1) * dependenceFactor) / countIn;
        }
        //cerr << "P(obs|" << genotype << ") = " << probObsGivenGt << endl;
        return isinf(probObsGivenGt) ? 0 : probObsGivenGt;
    }

}


vector<pair<Genotype*, long double> >
probObservedAllelesGivenGenotypes(
        Sample& sample,
        vector<Genotype*>& genotypes,
        double dependenceFactor,
        bool useMapQ,
        Bias& observationBias,
        bool standardGLs,
        vector<Allele>& genotypeAlleles,
        Contamination& contaminations,
        map<string, double>& freqs
    ) {
    vector<pair<Genotype*, long double> > results;
    for (vector<Genotype*>::iterator g = genotypes.begin(); g != genotypes.end(); ++g) {
        results.push_back(
	    make_pair(*g,
                  probObservedAllelesGivenGenotype(
                      sample,
                      **g,
                      dependenceFactor,
                      useMapQ,
                      observationBias,
                      standardGLs,
                      genotypeAlleles,
                      contaminations,
                      freqs)));
    }
    return results;
}
